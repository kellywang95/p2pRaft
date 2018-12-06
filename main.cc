
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
	heartbeatTimer->start(100);
	connect(heartbeatTimer, SIGNAL(timeout()),
            this, SLOT(heartbeatHandler())); 

	state = QString::fromStdString("CANDIDATE");
	startRaft = false;
	for (int i = udpSocket->myPortMin; i <= udpSocket->myPortMax; i++) {
		nodeStates[i] = QString::fromStdString("CANDIDATE");
	}

	nextSeqNo = 0;
	nextSeqToShow = 0;
	currentVote = 0;

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
	QStringList messageParts = message.split(" ");
	qDebug() << "parsed input: ";
 	QStringList::const_iterator constIterator;
	for (constIterator = messageParts.constBegin(); constIterator != messageParts.constEnd();
           ++constIterator) {
		qDebug() << (*constIterator).toLocal8Bit().constData() ;
	}
	if (messageParts.size() == 1) {
		if (message == QString::fromStdString("START")) {
			startRaft = true;
		} else if (message == QString::fromStdString("GET_CHAT")) {
			this->textview->append("-----------------");
			this->textview->append("----History:-----");
			this->textview->append("-----------------");
			for (qint32 i = 0; i < committedMsgs.size(); i++) {
				this->textview->append(committedMsgs[i]["Origin"].toString() + ">: " + committedMsgs[i]["ChatText"].toString());
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
			for (QMap<quint32, QString>::const_iterator iter = nodeStates.begin(); iter != nodeStates.end(); ++iter) {
				this->textview->append(QString::number(iter.key()) + ": " + iter.value());
			}
			this->textview->append("-----------------");
			this->textview->append("----Status End---");
			this->textview->append("-----------------");
		}
	} else if (messageParts.size() > 1) {
		if (messageParts[0] == QString::fromStdString("MSG")) {
			QVariantMap newMsg;
			QStringList newParts = messageParts;
			newParts.pop_front();
			newMsg["Origin"] = udpSocket->originName;
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
			handleApproveMsg(msgMap);
		} else if (msgMap["Type"] == QString::fromStdString("MsgCommit")) {
			done = true;
			handleCommitMsg(msgMap);
		} else if (msgMap["Type"] == QString::fromStdString("HeartBeat")) {
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
	// qDebug() << "timeout!";

	// reset currentVote and VoteToMe
	currentVote = 0;
	VoteToMe.clear();
	leaderPort = 0;
	
	//PROPOSE
	proposeLeader();

	timeoutTimer->start(500);
}

void ChatDialog::heartbeatHandler() {
	// qDebug() << "in heartbeatHandler!";

	if (state == QString::fromStdString("LEADER")) {
		qDebug() << "send heartBeat!";
		sendHeartbeat();
		timeoutTimer->start(500);
	}
	heartbeatTimer->start(100);
}

void ChatDialog::addToUncommittedMsgs(QVariantMap &qMap) {
	QString message = qMap["ChatText"].toString();
	QString origin = qMap["Origin"].toString();
	quint32 seqNo = qMap["SeqNo"].toInt();
	
	if (message.isEmpty()) return;
	
	if (!uncommittedMsgs.contains(seqNo)){
		// TODO check if it's in committed??
		uncommittedMsgs.insert(seqNo, qMap);
	} else {
		return;
		// TODO ??
	}
}

void ChatDialog::removeFromUncommittedMsgs(QVariantMap &qMap) {
	QString message = qMap["ChatText"].toString();
	QString origin = qMap["Origin"].toString();
	quint32 seqNo = qMap["SeqNo"].toInt();
	
	if (message.isEmpty()) return;
	
	if (uncommittedMsgs.contains(seqNo)){
		uncommittedMsgs.remove(seqNo);
	}
}


void ChatDialog:: addToCommittedMsgs(QVariantMap &qMap) {
	QString message = qMap["ChatText"].toString();
	QString origin = qMap["Origin"].toString();
	quint32 seqNo = qMap["SeqNo"].toInt();
	
	if (message.isEmpty()) return;
	
	if (committedMsgs.contains(seqNo)){
		return;
	}

	committedMsgs.insert(seqNo, qMap);
	// if any new messages, display in window
	while (committedMsgs.contains(nextSeqToShow)) {
		this->textview->append(committedMsgs[nextSeqToShow]["Origin"].toString() + ">: " + committedMsgs[nextSeqToShow]["ChatText"].toString());
		nextSeqToShow++;
	}
}



void ChatDialog::proposeMsg(QVariantMap &qMap) {
	// if is leader: send to all other nodes
	if (state == QString::fromStdString("LEADER")) {
		qMap["SeqNo"] = QString::number(nextSeqNo);
		nextSeqNo++;
		sendMsgToOthers(qMap);
	} else {
		// if not leader, send to leader only
		udpSocket->sendUdpDatagram(qMap, leaderPort);
	}
	

}


void ChatDialog::handleProposeMsg(QVariantMap &qMap) {
	QString origin = qMap["Origin"].toString();
	quint32 originPort = origin.toInt();
	if (state == QString::fromStdString("LEADER")) {
		proposeMsg(qMap);

	} else if (nodeStates[originPort] ==  QString::fromStdString("LEADER")) {
		// elif origin is leader: send approveMsg
		approveMsg(qMap);
	}

}


void ChatDialog::approveMsg(QVariantMap &qMap) {
	QVariantMap approveMap = qMap;
	approveMap["Type"] = QString::fromStdString("MsgApprove");
	udpSocket->sendUdpDatagram(approveMap, leaderPort);
}

void ChatDialog::handleApproveMsg(QVariantMap &qMap){
	// add to msgApproves
	quint32 seqNo = qMap["SeqNo"].toInt();
	QString origin = qMap["Origin"].toString();
	if (!msgApproves.contains(seqNo)) {
		QStringList initList;
		msgApproves.insert(seqNo,initList);
	}
	if (!msgApproves[seqNo].contains(origin)) {
		msgApproves[seqNo].append(origin);
	}
	

	// check if votes reach majority
	// call commitMsg
	if (msgApproves[seqNo].length() >= 3) {
		commitMsg(qMap);
	}



}
void ChatDialog::commitMsg(QVariantMap &qMap) {
	qDebug() << "in commitMsg::::" << qMap["ChatText"].toString();
	// add to committedMsgs
	addToCommittedMsgs(qMap);

	// send to others
	QVariantMap commitMap = qMap;
	commitMap["Type"] = QString::fromStdString("MsgCommit");

	sendMsgToOthers(commitMap);

	// remove from uncommittedMsgs
	removeFromUncommittedMsgs(qMap);


}

void ChatDialog::handleCommitMsg(QVariantMap &qMap) {
	qDebug() << "in handleCommitMsg" << qMap["ChatText"].toString();
	//quint32 seqNo = qMap["SeqNo"].toInt();
	//QString message = qMap["ChatText"].toString();

	addToCommittedMsgs(qMap);
	removeFromUncommittedMsgs(qMap);
}


void ChatDialog::proposeLeader() {
	qDebug() << "propose itself to be leader";

	currentVote = udpSocket->myPort;
	VoteToMe.append(udpSocket->originName);

	QVariantMap qMap;
	qMap["Type"] = QString::fromStdString("LeaderPropose");
	qMap["Origin"] = QString::number(udpSocket->myPort);

	// send LeaderPropose msg to every other nodes
	sendMsgToOthers(qMap);
}


void ChatDialog::handleProposeLeader(quint32 port) {
	// check if it has voted to anywhere in this round
	if (currentVote == 0) {
		// if not voted yet in this round
		approveLeader(port);
	} else {
		// ignore this proposal
		return;
	}

	// ??? heartbeatTimer->start(100);
}


void ChatDialog::approveLeader(quint32 port) {
	// send LeaderApprove Msg
	QVariantMap qMap;
	qMap["Type"] = QString::fromStdString("LeaderApprove");
	qMap["Origin"] = QString::number(udpSocket->myPort);
	udpSocket->sendUdpDatagram(qMap, port);
}


void ChatDialog::handleApproveLeader(quint32 port) {
	// add approves to VoteToMe
	QString fromPort = QString::number(port);
	if (VoteToMe.contains(fromPort)) return;
	VoteToMe.append(fromPort);

	// check if reach majority
	if (VoteToMe.length() >=3 ) {
		// set myself to be the leader and commit to others
		state = QString::fromStdString("LEADER");
		commitLeader(udpSocket->myPort);
		leaderPort = udpSocket->myPort;
	}
}

void ChatDialog::commitLeader(quint32 port) {
	QVariantMap qMap;
	qMap["Type"] = QString::fromStdString("LeaderCommit");
	qMap["Origin"] = QString::number(port);

	//send leaderCommit msg to others
	sendMsgToOthers(qMap);
}


void ChatDialog::handleCommitLeader(quint32 port) {
	// set leaderPort to be the new Leader
	leaderPort = port;

}


void ChatDialog::sendHeartbeat() {
	 //  leader send heartbeat to others
	QVariantMap qMap;
	qMap["Type"] = QString::fromStdString("HeartBeat");
	qMap["Origin"] = QString::number(udpSocket->myPort);

	sendMsgToOthers(qMap);

}

void ChatDialog::handleHeartbeat(quint32 port) {

	if (port != leaderPort) {
		leaderPort = port;
	}

	// reset timeout
	timeoutTimer->start(500);

}

// send msg to all the other nodes
void ChatDialog::sendMsgToOthers(QVariantMap &qMap) {
	for (int p = udpSocket->myPortMin; p <= udpSocket->myPortMax; p++) {
		if (p != udpSocket->myPort) {
			udpSocket->sendUdpDatagram(qMap, p);
		}
	}

}
void ChatDialog::sendAllMsg(quint32 port) {
	return;

	QMap<QString, QMap<quint32, QVariantMap> > qMap;
	qMap["Type"][0]["Type"] = "ALLMSG";
	qMap["committed"] = committedMsgs;
	qMap["Uncommitted"] = uncommittedMsgs;
	
	qDebug() << QString::number(port);


}


void ChatDialog::handleAllMsg(QMap<QString, QMap<quint32, QVariantMap> >&qMap){

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
	myPortMax = myPortMin + 4;
	// get host address
	HostAddress = QHostAddress(QHostAddress::LocalHost);
    QHostInfo info;
}


NetSocket::~NetSocket() {}

bool NetSocket::bind()
{
	// Try to bind to each of the range myPortMin..myPortMax in turn.
	for (int p = myPortMin; p <= myPortMax; p++) {
		if (QUdpSocket::bind(p)) {
			qDebug() << "bound to UDP port " << p;
			myPort = p;
			originName = QString::number(myPort);
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
	QDataStream outStream(&mapData, QIODevice::WriteOnly);
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

