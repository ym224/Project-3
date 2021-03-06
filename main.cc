
#include <unistd.h>

#include <QVBoxLayout>
#include <QApplication>
#include <QDebug>

#include "main.hh"

ChatDialog::ChatDialog()
{
	setWindowTitle("P2Papp");

	// Read-only text box where we display messages from everyone.
	// This widget expands both horizontally and vertically.
	textview = new QTextEdit(this);
	textview->setReadOnly(true);

	// Small text-entry box the user can enter messages.
	// This widget normally expands only horizontally,
	// leaving extra vertical space for the textview widget.
	//
	// You might change this into a read/write QTextEdit,
	// so that the user can easily enter multi-line messages.
	textline = new QLineEdit(this);

	// Lay out the widgets to appear in the main window.
	// For Qt widget and layout concepts see:
	// http://doc.qt.nokia.com/4.7-snapshot/widgets-and-layouts.html
	QVBoxLayout *layout = new QVBoxLayout();
	layout->addWidget(textview);
	layout->addWidget(textline);
	setLayout(layout);

	// Create a UDP network socket
	socket = new NetSocket();
	if (!socket->bind()) {
		exit(1);
	}
	
	// generate unique ID for local origin
	originId = generateOriginId(socket->getPort());
	
	setWindowTitle(originId);
	
	// add adjacent ports as neighbors
	addNeighbors();
	
	// initialize timeout timer
	timer = new QTimer(this);
	connect(timer, SIGNAL(timeout()), this, SLOT(timeoutHandler()));

	// initialize anti-entropy timer
	antiEntropyTimer = new QTimer(this);
	connect(antiEntropyTimer, SIGNAL(timeout()), this, SLOT(antiEntropyHandler()));

	seqNum = 0;
	timer_started = false;
	
	// Register a callback on the textline's returnPressed signal
	// so that we can send the message entered by the user.
	connect(textline, SIGNAL(returnPressed()), this, SLOT(gotReturnPressed()));

	// connect the readyRead signal in QUdpSocket
	connect(socket, SIGNAL(readyRead()), this, SLOT(readDatagrams()));
}


void ChatDialog::addNeighbors()
{
	int port = socket->getPort();
	if (port == socket->getMyPortMin()){
		neighbors.append(port + 1);
		qDebug() << "INFO: adding neighbor: " << neighbors.first();
	}
	if (port == socket->getMyPortMax()){
		neighbors.append(port - 1);
		qDebug() << "INFO: adding neighbor: " << neighbors.first();		
	}
	if (port < socket->getMyPortMax() && port > socket->getMyPortMin()){
		neighbors.append(port - 1);
		qDebug() << "INFO: adding neighbor: " << neighbors.first();				
		neighbors.append(port + 1);
		qDebug() << "INFO: adding neighbor: " << neighbors.last();				
	}
}

// called when a new payload of network data has arrived on socket
void ChatDialog::readDatagrams()
{
	QHostAddress senderAddr;
	quint16 senderPort;
	// receive and serialize data from socket
	QVariantMap message = socket->readData(senderAddr, senderPort);
	
	// check if rumorMessage of statusMessage
	if (message.contains("Want")){
		qDebug() << "INFO: Received a status message";
		// process status message
		processStatusMessage(message, senderPort);
	}
	// process rumor message
	else if (message.contains("ChatText")){
		qDebug() << "INFO: Received a rumor message";
		processRumorMessage(message, senderPort);
	}
	else {
		qDebug() << "ERROR: Received neither a rumor nor a status message";
		return;
	}
}

// compare peer to local status
// if you have message not seen by peer, rumorMonger
// if peer has message not seen by you, send status 
void ChatDialog::processStatusMessage(QVariantMap peerStatusMessage, quint16 senderPort)
{
	QMap<QString, QVariant> peerStatusValue = peerStatusMessage["Want"].toMap();
	QMap<QString, QVariant> statusValue = statusMessage["Want"].toMap();
	qDebug() << "Peer status is" << peerStatusValue;
        qDebug() << "Local status is" << statusValue;

	timer->stop();
	timer_started = false;
	bool insync = true;
	for (QString origin: statusValue.keys()){
		// if local ahead of remote, send new rumor
		if (!peerStatusValue.contains(origin) || peerStatusValue[origin].toUInt() < statusValue[origin].toUInt()){
		        qDebug() << "INFO: Local ahead of remote";
		        quint32 peerSeqNum = 1;
		        if (peerStatusValue.contains(origin)) {
		        	peerSeqNum = peerStatusValue[origin].toUInt();
		        }
		        // update local messages map
		        QVariantMap rumorMessage = messages[origin].value(seqNum);
		        socket->sendData(serializeMessage(rumorMessage), senderPort);
		        lastRumorMessage = rumorMessage;
		        timer->start(TIMEOUT);
		        timer_started = true;
			insync = false;
		}
	}
	for (QString origin: peerStatusValue.keys()){
		// local behind remote, send status
		if (!statusValue.contains(origin) || statusValue[origin].toUInt() < peerStatusValue[origin].toUInt()){
			qDebug() << "INFO: Remote ahead of local";
			socket->sendData(serializeMessage(statusMessage), senderPort);	
			insync = false;
		}
	}
	// if local in sync with remote, flip coin, and rumorMonger if heads	
	if (insync && qrand() > 1/2 * RAND_MAX) {
		qDebug() << "INFO: Local and Remote in sync. Rumor mongering with last message.";	
		rumorMonger(lastRumorMessage);
	}
}

void ChatDialog::processRumorMessage(QVariantMap rumorMessage, quint16 senderPort)
{
	// get origin and seqnum
	// terminate if origin is self
	QString origin = rumorMessage["Origin"].toString();
	quint32 seqNum = rumorMessage["SeqNo"].toInt();
	QString chatText = rumorMessage["ChatText"].toString();
	
	timer->stop();
	timer_started = false;
	
	if (origin == this->originId) {
		qDebug() << "INFO: Received rumor message originated locally";
		return;
	}
	qDebug() << "INFO: Received rumor message from" << origin << "with seqNum" << seqNum;
	
	// insert rumor Message to local messages map
	QMap<quint32, QVariantMap> seqNumMap;
	seqNumMap[seqNum] = rumorMessage;
	messages[origin] = seqNumMap;		

	// update status message if new
	if (updateStatusMessage(origin, seqNum)){
		// rumorMonger with received message
		rumorMonger(rumorMessage);
	
		// add text to textView
		textview->append(origin + ": " + chatText);
	}
	else {
		qDebug() << "INFO: Received rumor message out of sequence";
	}
	// send statusMessage as ack
	socket->sendData(serializeMessage(statusMessage), senderPort);
}
 
QString ChatDialog::generateOriginId(int port)
{	
	int id = port - socket->getMyPortMin();
	QString qString(QHostInfo::localHostName() + "-" + QString::number(id));
	qDebug() << "INFO: Generated origin id: " << qString;
	return qString;
}

void ChatDialog::gotReturnPressed()
{
	// Initially, just echo the string locally.
	// Insert some networking code here...
	qDebug() << "INFO: User entered " << textline->text();
	
	sendMessage(textline->text());
	textview->append(originId + ": " + textline->text());
	
	// Clear the textline to get ready for the next input message.
	textline->clear();
}

void ChatDialog::sendMessage(QString text)
{	
	seqNum++;
	QVariantMap message = createRumorMessage(text, originId, seqNum);
	
	// add originId, seqNum and message to map to keep track of read messages
	QMap<quint32, QVariantMap> seqMessageMap;
	seqMessageMap[seqNum] = message;
	messages[originId] = seqMessageMap;
 	qDebug() << "INFO: Updated local messages with seqNum/rumorMessage " << messages;
 	
	// update status message with next seqNum for local origin
	updateStatusMessage(originId, seqNum);
	
	// rumor monger to random neighbor
	rumorMonger(message);
}

void ChatDialog::rumorMonger(QVariantMap message)
{
	// pick a random neighbor and send rumorMessage
	quint32 receiverPort = (quint32)neighbors.at(qrand() % neighbors.size());
	qDebug() << "INFO: Gossiping to neighbor " << receiverPort;
	socket->sendData(serializeMessage(message), receiverPort);
	lastRumorMessage = message;
	timer->start(TIMEOUT);
	timer_started = true;
}

void ChatDialog::timeoutHandler()
{
	if (timer_started) {
		qDebug() << "INFO: In timeoutHandler";
		// rumor monger with last sent message
		rumorMonger(lastRumorMessage);
	}
}


void ChatDialog::antiEntropyHandler()
{
	qDebug() << "INFO: In antiEntropyHandler";
	int rand_idx = qrand() % neighbors.size();
	QVariantMap status;
	socket->sendData(serializeMessage(status), (quint32)neighbors.at(rand_idx));

	// reset timer
	antiEntropyTimer->start(ANTIENTROPY_TIMEOUT);
}

QByteArray ChatDialog::serializeMessage(QVariantMap message)
{
	QByteArray bytes;
	// serialize message using QDataStream
	QDataStream stream(&bytes, QIODevice::ReadWrite);
	stream << message;
	return bytes;
}

QVariantMap ChatDialog::createRumorMessage(QString chatText, QString origin, quint32 seqNum)
{
	QVariantMap rumorMessage;
	rumorMessage["ChatText"] = chatText;
	rumorMessage["Origin"] = origin;
	rumorMessage["SeqNo"] = seqNum;
	return rumorMessage;
} 

QVariantMap ChatDialog::createStatusMessage(QString origin, quint32 seqNum)
{
	QVariantMap statusMessage;
	QVariantMap statusValue;
	statusValue[origin] = seqNum;
	statusMessage["Want"] = statusValue;	
	return statusMessage;
}

bool ChatDialog::updateStatusMessage(QString origin, quint32 seqNum)
{	
	qDebug() << "INFO: In updating local status for origin" << origin << "and seqnum" << seqNum;
	QMap<QString, QVariant> statusValue = (statusMessage["Want"]).toMap();
	qDebug() << "Local status message before update is" << statusMessage;	
	// increment seqNum if local status contains expected seqNum or if origin not yet seen
	if (statusValue[origin] == seqNum || seqNum == 1) {
		statusValue[origin] = seqNum + 1;
		statusMessage["Want"] = statusValue;
	qDebug() << "Local status message after update is" << statusMessage;
		return true;
	}
	return false;
}

NetSocket::NetSocket()
{
	// Pick a range of four UDP ports to try to allocate by default,
	// computed based on my Unix user ID.
	// This makes it trivial for up to four P2Papp instances per user
	// to find each other on the same host,
	// barring UDP port conflicts with other applications
	// (which are quite possible).
	// We use the range from 32768 to 49151 for this purpose.
	myPortMin = 32768 + (getuid() % 4096)*4;
	myPortMax = myPortMin + 3;
}

bool NetSocket::bind()
{
	// Try to bind to each of the range myPortMin..myPortMax in turn.
	for (int p = myPortMin; p <= myPortMax; p++) {
		if (QUdpSocket::bind(p)) {
			qDebug() << "INFO: bound to UDP port " << p;
			port = p;
			return true;
		}
	}

	qDebug() << "Oops, no ports in my default range " << myPortMin
		<< "-" << myPortMax << " available";
	return false;
}


int NetSocket::getPort() 
{ 
	return port; 
}
	
	
int NetSocket::getMyPortMin() 
{ 
	return myPortMin; 
}


int NetSocket::getMyPortMax() 
{ 
	return myPortMax; 
}

void NetSocket::sendData(QByteArray bytes, quint32 receiverPort)
{
	writeDatagram(bytes, QHostAddress(QHostAddress::LocalHost), receiverPort);
}

QVariantMap NetSocket::readData(QHostAddress senderAddr, quint16 senderPort)
{
	QVariantMap rumorMessage;
	while (this->hasPendingDatagrams()) {
		QByteArray bytes;
		bytes.resize(this->pendingDatagramSize());
		this->readDatagram(bytes.data(), bytes.size(), &senderAddr, &senderPort);
		// deserialize from bytes to QVariantMap
		QDataStream stream(&bytes, QIODevice::ReadOnly);
		stream >> rumorMessage;
		qDebug() << "INFO: Receiving " << rumorMessage << " from " << senderPort;
	}
	return rumorMessage;
}


int main(int argc, char **argv)
{
	// Initialize Qt toolkit
	QApplication app(argc,argv);

	// Create an initial chat dialog window
	ChatDialog dialog;
	dialog.show();

	// Enter the Qt main loop; everything else is event driven
	return app.exec();
}

