
#include <unistd.h>

#include <QVBoxLayout>
#include <QApplication>
#include <QDebug>
#include <QHostAddress>
#include <QHostInfo>
#include <QDateTime>
#include <QTimer>
#include <QMutex>
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
	udpSocket = new NetSocket(this);
	if (!udpSocket->bind())
		exit(1);
	
	timeoutTimer = new QTimer(this);
	timeoutTimer->start(500);
	connect(timeoutTimer, SIGNAL(timeout()),
            this, SLOT(timeoutHandler())); 

	heartbeatTimer = new QTimer(this);
	connect(heartbeatTimer, SIGNAL(timeout()),
            this, SLOT(heartbeatHandler())); 

	state = QString::fromStdString("CANDIDATE");
	startRaft = false;
	for (int i = udpSocket.myPortMin; i <= udpSocket.myPortMax; i++) {
		nodeStates[i] = QString::fromStdString("CANDIDATE");
	}

	nextSeqNo = 0;
	nextSeqToShow = 0;

	// Register a callback on the textline's returnPressed signal
	// so that we can send the message entered by the user.
	connect(textline, SIGNAL(returnPressed()),
		this, SLOT(gotReturnPressed()));
	// Register a callback on for the server to read msg.
	connect(udpSocket, SIGNAL(readyRead()),
		this, SLOT(gotReadyRead()));
}

void ChatDialog::gotReturnPressed()
{
	QString message = textline->text();
	auto messageParts = message.split(" ");
	qDebug("parsed input: ");
	for (auto str : messageParts) {
		qDebug(str);
	}

	if (messageParts.size() == 1) {
		if (message == QString::fromStdString("START")) {
			startRaft = true;
		} else if (message == QString::fromStdString("GET_CHAT")) {
			this->textview->append("-----------------");
			this->textview->append("----History:-----");
			this->textview->append("-----------------");
			for (qint32 i = 0; i < commitedMsgs.size(); i++) {
				this->textview->append(commitedMsgs[i]["Origin"] + ">: " + commitedMsgs[i]["ChatText"]);
			}
			this->textview->append("-----------------");
			this->textview->append("---History End---");
			this->textview->append("-----------------");
		} else if (message == QString::fromStdString("STOP")) {
			startRaft = false;
		} else if (message == QString::fromStdString("GET_NODES")) {
			this->textview->append("-----------------");
			this->textview->append("-----Status:-----");
			this->textview->append("-----------------");
			for (auto it : nodeStates) {
				this->textview->append(QString::number(it.first) + ": " + it.second);
			}
			this->textview->append("-----------------");
			this->textview->append("----Status End---");
			this->textview->append("-----------------");
		}
	} else if (messageParts.size() > 1) {
		if (messageParts[0] == QString::fromStdString("MSG")) {
			QVariantMap newMsg;
			auto newParts = messageParts;
			newParts.pop_front();
			newMsg["Origin"] = udpSocket.originName;
			newMsg["ChatText"] = newParts.join(" ");
			proposeMsg(newMsg);
		} else if (messageParts.size() == 2) {
			if (messageParts[0] == QString::fromStdString("DROP")){
				declineNodes[messageParts[1].toInt()] = true;
			} else if (messageParts[0] == QString::fromStdString("RESTORE")){
				declineNodes[messageParts[1].toInt()] = false;
			}
		}
	}
	
	// Clear the textline to get ready for the next input message.
	textline->clear();
}

void ChatDialog::gotReadyRead() {
	QVariantMap msgMap;
	QMap<QString, QMap<quint32, QVariantMap> > allMsgsMap;
	QHostAddress serverAdd;
	quint16 serverPort;

RECV:
// TODO if (!startRaft) no proto opp;
	QByteArray mapData(udpSocket->pendingDatagramSize(), Qt::Uninitialized);
	udpSocket->readDatagram(mapData.data(), mapData.size(), &serverAdd, &serverPort);
	QDataStream msgMapStream(&mapData, QIODevice::ReadOnly);
	QDataStream allMsgsMapStream(&mapData, QIODevice::ReadOnly);
	msgMapStream >> (msgMap);
	allMsgsMapStream >> (allMsgsMap);

	bool done = false;

	if (msgMap.contains("Type"))
	{
		if (msgMap["Type"] == QString::fromStdString("LeaderPropose")) {
			done = true;
			handleProposeLeader(msgMap["Origin"].toInt());
		} else if (msgMap["Type"] == QString::fromStdString("LeaderApprove")) {
			done = true;
			handleApproveLeader(msgMap["Origin"].toInt());
		} else if (msgMap["Type"] == QString::fromStdString("LeaderCommit")) {
			done = true;
			handleCommitLeader(msgMap["Origin"].toInt());
		} else if (msgMap["Type"] == QString::fromStdString("MsgPropose")) {
			done = true;
			handleProposeMsg(msgMap);
		} else if (msgMap["Type"] == QString::fromStdString("MsgApprove")) {
			done = true;
			handleApproveLeader(msgMap);
		} else if (msgMap["Type"] == QString::fromStdString("MsgCommit")) {
			done = true;
			handleCommitLeader(msgMap);
		} else if (msgMap["Type"] == QString::fromStdString("heartBeat")) {
			done = true;
			handleHeartbeat(msgMap["Origin"].toInt());
		}
	}

	if (!done) {
		if (allMsgsMap.contains("Type") && allMsgsMap["Type"][0]["Type"] == "AllMsgs") {
			handleAllMsg(allMsgsMap);
		}
	}

    if (udpSocket->hasPendingDatagrams()) {
		goto RECV;
	}
}


void ChatDialog::timeoutHandler() {
	
	// TODO Check state and decide wat 2 do
	qDebug("timeout!");
	timeoutTimer->start(500);
}

void ChatDialog::heartbeatHandler() {
	if (state != QString::fromStdString("LEADER")) {
		heartbeatTimer->stop();
	}
	qDebug("heartBeat!");
	sendHeartbeat();
	heartbeatTimer->start(100);
	timeoutTimer->start(500);
}

void ChatDialog::addToUncommitedMsgs(QVariantMap &qMap) {
	QString message = qMap["ChatText"].toString();
	QString origin = qMap["Origin"].toString();
	quint32 seqNo = qMap["SeqNo"].toInt();
	
	if (message.isEmpty()) return;
	
	if (!uncommitedMsgs.contains(seqNo)){
		// TODO check if it's in commited??
		uncommitedMsgs.insert(seqNo, qMap);
	} else {
		return;
		// TODO ??
	}
}
void ChatDialog::addToCommitedMsgs(QVariantMap &qMap) {
	QString message = qMap["ChatText"].toString();
	QString origin = qMap["Origin"].toString();
	quint32 seqNo = qMap["SeqNo"].toInt();
	
	if (message.isEmpty()) return;
	
	if (!commitedMsgs.contains(seqNo)){
		// TODO check if it's in uncommited??
		commitedMsgs.insert(seqNo, qMap);
	} else {
		return;
		// TODO ??
	}
	
	while (commitedMsgs.contains(nextSeqToShow)) {
		this->textview->append(commitedMsgs[nextSeqToShow]["Origin"] + ">: " + commitedMsgs[nextSeqToShow]["ChatText"]);
		nextSeqToShow++;
	}
}



void ChatDialog::proposeMsg(QVariantMap &qMap) {
}
void ChatDialog::handleProposeMsg(QVariantMap &qMap) {
}
void ChatDialog::approveMsg(QVariantMap &qMap) {
}
void ChatDialog::handleApproveMsg(QVariantMap &qMap){
}
void ChatDialog::commitMsg(QVariantMap &qMap) {
}
void ChatDialog::handleCommitMsg(QVariantMap &qMap) {
}

void ChatDialog::proposeLeader() {
}
void ChatDialog::handleProposeLeader(qint32 port) {
	if (TODO: reached major) {
		state = QString::fromStdString("LEADER");
		heartbeatTimer->start(100);
	}
}
void ChatDialog::approveLeader(qint32 port) {
}
void ChatDialog::handleApproveLeader(qint32 port) {
}
void ChatDialog::sendHeartbeat() {
}
void ChatDialog::handleHeartbeat(qint32 port) {	
}

void ChatDialog::sendAllMsg(qint32 port) {
	// qMap["Type"][0]["Type"] = "ALLMSG";
	// qMap["Commited"] =
	// qMap["Uncommited"] =
}
void ChatDialog::handleAllMsg(QMap<QString, QMap<quint32, QVariantMap> >&qMap){
}

void ChatDialog::sendHeartbeat() {
}
void ChatDialog::handleHeartbeat(qint32 port) {
}

NetSocket::NetSocket(QObject *parent = NULL): QUdpSocket(parent)
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
	// get host address
	HostAddress = QHostAddress(QHostAddress::LocalHost);
    QHostInfo info;
    originName = QString::number(HostAddress);
}


NetSocket::~NetSocket() {}

bool NetSocket::bind()
{
	// Try to bind to each of the range myPortMin..myPortMax in turn.
	for (int p = myPortMin; p <= myPortMax; p++) {
		if (QUdpSocket::bind(p)) {
			qDebug() << "bound to UDP port " << p;
			myPort = p;
			return true;
		}
	}

	qDebug() << "Oops, no ports in my default range " << myPortMin
		<< "-" << myPortMax << " available";
	return false;
}

void NetSocket::sendUdpDatagram(const QVariantMap &qMap, int port) {
	if (qMap.isEmpty()) return;

	QByteArray mapData;
	QDataStream outStream(&qMap, QIODevice::WriteOnly);
	outStream << qMap;
	this->writeDatagram(mapData, HostAddress, port);
}

void NetSocket::sendUdpDatagram(const QMap<QString, QMap<quint32, QVariantMap> >&qMap, int port) {
	if (qMap.isEmpty()) return;

	QByteArray mapData;
	QDataStream outStream(&mapData, QIODevice::WriteOnly);
	outStream << qMap;
	this->writeDatagram(mapData, HostAddress, port);
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

