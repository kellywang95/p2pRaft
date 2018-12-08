
#include <unistd.h>

#include <QVBoxLayout>
#include <QApplication>
#include <QDebug>
#include <QHostAddress>
#include <QHostInfo>
#include <QDateTime>
#include <QTimer>
#include <QMutex>
#include <string>
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
	timeoutTimer->start(1500 + genRandNum() % 2000);
	connect(timeoutTimer, SIGNAL(timeout()),
            this, SLOT(timeoutHandler())); 

	heartbeatTimer = new QTimer(this);
	heartbeatTimer->start(800);
	connect(heartbeatTimer, SIGNAL(timeout()),
            this, SLOT(heartbeatHandler())); 

	restoreWaitingTimer = new QTimer(this);
	connect(restoreWaitingTimer, SIGNAL(timeout()),
            this, SLOT(restoreTimeoutHandler())); 

	startRaft = true;
	for (int i = udpSocket->myPortMin; i <= udpSocket->myPortMax; i++) {
		nodeStates[i] = QString::fromStdString("CANDIDATE");
	}

	nextSeqNo = 0;
	currentVote = 0;
	voteLeaderRound = 0;

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
			for (QMap<quint32, QVariantMap>::const_iterator iter = committedMsgs.begin(); iter != committedMsgs.end(); ++iter) {
				this->textview->append(iter.value()["Origin"].toString() + ">: " + iter.value()["ChatText"].toString());
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
			if (!startRaft) {
				unsendMsgs.append(newMsg["ChatText"].toString());
				textline->clear();
				return;
			}
			proposeMsg(newMsg);
		} else if (messageParts.size() == 2) {
			if (messageParts[0] == QString::fromStdString("DROP")){
				declineNodes[messageParts[1].toInt()] = true;
				if (!startRaft) {
					textline->clear();
					return;
				}
			} else if (messageParts[0] == QString::fromStdString("RESTORE")){
				declineNodes[messageParts[1].toInt()] = false;
				if (!startRaft) {
					textline->clear();
					return;
				}
				requestAllMsg();
				restoreWaitingTimer->start(1200);
				// restore dropped msgs from nodeId
				
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
	QByteArray mapData(udpSocket->pendingDatagramSize(), Qt::Uninitialized);
	udpSocket->readDatagram(mapData.data(), mapData.size(), &serverAdd, &serverPort);
	QDataStream msgMapStream(&mapData, QIODevice::ReadOnly);
	QDataStream allMsgsMapStream(&mapData, QIODevice::ReadOnly);
	msgMapStream >> (msgMap);
	allMsgsMapStream >> (allMsgsMap);

	bool done = false;
	if (!startRaft) {
		goto SKIP;
	}

	if (declineNodes[serverPort]) {
		qDebug() << "decline msg sent from" << msgMap["Origin"];
		if (msgMap["SeqNo"].toInt() >= nextSeqNo) {
			nextSeqNo = msgMap["SeqNo"].toInt()+1;
		}
		goto SKIP;
	}

	if (msgMap.contains("Type"))
	{
		qDebug() << "recv:";
		qDebug() << msgMap;
		if (msgMap["Type"] == QString::fromStdString("LeaderPropose")) {
			done = true;
			handleProposeLeader(msgMap["Origin"].toInt(), msgMap["Round"].toInt());
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
		} else if (msgMap["Type"] == QString::fromStdString("RequestAllMsg")) {
			done = true;
			qDebug() << "Got RequestAllMsg from " << msgMap["Origin"];
			handleRequestAllMsg(msgMap["Origin"].toInt());
		}
	}

	if (!done) {
		qDebug() << "recv:";
		qDebug() << allMsgsMap;
		if (allMsgsMap.contains("Type") && allMsgsMap["Type"][0]["Type"] == "AllMsgs") {
			handleAllMsg(allMsgsMap);
		}
	}

SKIP:
    if (udpSocket->hasPendingDatagrams()) {
		goto RECV;
	}
}


void ChatDialog::timeoutHandler() {
	
	// TODO Check state and decide wat 2 do
	// qDebug() << "timeout!";

	// reset currentVote and VoteToMe
	voteLeaderRound++;
	currentVote = 0;
	VoteToMe.clear();
	leaderPort = 0;
	for (int i = udpSocket->myPortMin; i <= udpSocket->myPortMax; i++) {
		nodeStates[i] = QString::fromStdString("CANDIDATE");
	}
	if (startRaft && leaderPort == 0) {
		//PROPOSE
		proposeLeader();
	}

	timeoutTimer->start(1500 + genRandNum() % 2000);
}

void ChatDialog::heartbeatHandler() {
	// qDebug() << "in heartbeatHandler!";

	if (startRaft) {
		if (myStates() == QString::fromStdString("LEADER")) {
			qDebug() << "send heartBeat!";
			sendHeartbeat();
			timeoutTimer->start(1500 + genRandNum() % 2000);
		}
		if (leaderPort != 0) resendMsgs();
	}
	
	heartbeatTimer->start(800);
}

void ChatDialog::addToUncommittedMsgs(const QVariantMap &qMap) {
	QVariantMap msg = qMap;
	QString message = msg["ChatText"].toString();
	QString origin = msg["Origin"].toString();
	quint32 seqNo = msg["SeqNo"].toInt();
	
	if (message.isEmpty()) return;
	
	if (!uncommittedMsgs.contains(seqNo)){
		// TODO check if it's in committed??
		uncommittedMsgs.insert(seqNo, msg);
	} else {
		return;
		// TODO ??
	}
}

void ChatDialog::removeFromUncommittedMsgs(const QVariantMap &qMap) {
	QString message = qMap["ChatText"].toString();
	QString origin = qMap["Origin"].toString();
	quint32 seqNo = qMap["SeqNo"].toInt();
	
	if (message.isEmpty()) return;
	
	if (uncommittedMsgs.contains(seqNo)){
		uncommittedMsgs.remove(seqNo);
	}
}


void ChatDialog:: addToCommittedMsgs(const QVariantMap &qMap) {
	QVariantMap msg = qMap;
	QString message = msg["ChatText"].toString();
	QString origin = msg["Origin"].toString();
	quint32 seqNo = msg["SeqNo"].toInt();
	
	if (message.isEmpty()) return;
	
	if (committedMsgs.contains(seqNo)){
		return;
	}

	committedMsgs.insert(seqNo, msg);
	
	this->textview->append(committedMsgs[seqNo]["Origin"].toString() + ">: " + committedMsgs[seqNo]["ChatText"].toString());
	// if any new messages, display in window
	/* while (committedMsgs.contains(nextSeqToShow)) {
		this->textview->append(committedMsgs[nextSeqToShow]["Origin"].toString() + ">: " + committedMsgs[nextSeqToShow]["ChatText"].toString());
		nextSeqToShow++;
	}
	*/

}



void ChatDialog::proposeMsg(const QVariantMap &qMap) {
	// if is leader: send to all other nodes
	qDebug() << "-----------------";
	qDebug() << "Enter proposeMsg:";
	qDebug() << qMap;
	qDebug() << "-----------------";
	QVariantMap proposeMap = qMap;
	proposeMap["Type"] = QString::fromStdString("MsgPropose");
	if (myStates() == QString::fromStdString("LEADER")) {
		proposeMap["SeqNo"] = QString::number(nextSeqNo);
		addToUncommittedMsgs(proposeMap);
		QStringList initList;
		msgApproves.insert(nextSeqNo,initList);
		msgApproves[nextSeqNo].append(udpSocket->originName);
		nextSeqNo++;
		sendMsgToOthers(proposeMap);
	} else {
		// if not leader, send to leader only
		udpSocket->sendUdpDatagram(proposeMap, leaderPort);
	}
}


void ChatDialog::handleProposeMsg(const QVariantMap &qMap) {
	qDebug() << "in handleProposeMsg";
	QVariantMap proposeMap = qMap;
	
	QString origin = qMap["Origin"].toString();
	quint32 originPort = origin.toInt();
	if (myStates() == QString::fromStdString("LEADER")) {
		proposeMsg(proposeMap);
	// } else if (nodeStates[originPort] ==  QString::fromStdString("LEADER")) {
	} else {

		addToUncommittedMsgs(proposeMap);
		if (proposeMap["SeqNo"].toInt() >= nextSeqNo) {
			nextSeqNo = proposeMap["SeqNo"].toInt() + 1;
		}
		approveMsg(proposeMap);
	}
}


void ChatDialog::approveMsg(const QVariantMap &qMap) {
	qDebug() << "in approveMsg";
	qDebug() << "leaderPort" << QString::number(leaderPort);
	qDebug() << qMap;
	QVariantMap approveMap = qMap;
	approveMap["Type"] = QString::fromStdString("MsgApprove");
	approveMap["Approver"] = udpSocket->originName;
	udpSocket->sendUdpDatagram(approveMap, leaderPort);
}

void ChatDialog::handleApproveMsg(const QVariantMap &qMap){
	// add to msgApproves
	QVariantMap approveMap = qMap;
	quint32 seqNo = approveMap["SeqNo"].toInt();
	QString approver = approveMap["Approver"].toString();
	if (!msgApproves[seqNo].contains(approver)) {
		msgApproves[seqNo].append(approver);
	}
	
	// check if votes reach majority
	// call commitMsg
	if (msgApproves[seqNo].length() >= 3) {
		commitMsg(approveMap);
	}
}
void ChatDialog::commitMsg(const QVariantMap &qMap) {
	QVariantMap commitMap = qMap;
	qDebug() << "in commitMsg" << qMap["ChatText"].toString();
	// add to committedMsgs
	addToCommittedMsgs(commitMap);

	// send to others
	commitMap["Type"] = QString::fromStdString("MsgCommit");

	sendMsgToOthers(commitMap);

	// remove from uncommittedMsgs
	removeFromUncommittedMsgs(commitMap);


}

void ChatDialog::handleCommitMsg(const QVariantMap &qMap) {
	QVariantMap commitMap = qMap;
	qDebug() << "in handleCommitMsg" << qMap["ChatText"].toString();
	//quint32 seqNo = qMap["SeqNo"].toInt();
	//QString message = qMap["ChatText"].toString();

	addToCommittedMsgs(commitMap);
	removeFromUncommittedMsgs(commitMap);
}


void ChatDialog::proposeLeader() {
	qDebug() << "propose itself to be leader";

	//currentVote = udpSocket->myPort;
	//VoteToMe.append(udpSocket->originName);

	QVariantMap qMap;
	qMap["Type"] = QString::fromStdString("LeaderPropose");
	qMap["Origin"] = QString::number(udpSocket->myPort);
	qMap["Round"] = QString::number(voteLeaderRound);

	// send LeaderPropose msg to every other nodes
	sendMsgToAll(qMap);
}


void ChatDialog::handleProposeLeader(quint32 port, quint32 round) {
	qDebug() << "in handleProposeLeader:::" << QString::number(port);
	// check if it has voted to anywhere in this round
	if (leaderPort == 0) {
		qDebug() << "no leaderPort";
		if (currentVote == 0 || (currentVote == udpSocket->myPort && round > voteLeaderRound) && VoteToMe.size() == 1) {
			if (currentVote == udpSocket->myPort) {
				qDebug() << "robbing";
				QStringList VoteToMeTMP;
				VoteToMe = VoteToMeTMP;
			}
			currentVote = port;
			approveLeader(port);
			qDebug() << "VoteToMe" << VoteToMe;
		}
	} else {
		qDebug() << "current leaderPort" << leaderPort;
	}
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
		updateLeader(udpSocket->myPort);
		commitLeader(udpSocket->myPort);
	}
}

void ChatDialog::commitLeader(quint32 port) {
	QVariantMap qMap;
	qMap["Type"] = QString::fromStdString("LeaderCommit");
	qMap["Origin"] = QString::number(port);

	//send leaderCommit msg to others
	sendMsgToAll(qMap);
}


void ChatDialog::handleCommitLeader(quint32 port) {
	// set leaderPort to be the new Leader
	updateLeader(port);

}


void ChatDialog::sendHeartbeat() {
	 //  leader send heartbeat to others
	QVariantMap qMap;
	qMap["Type"] = QString::fromStdString("HeartBeat");
	qMap["Origin"] = QString::number(udpSocket->myPort);

	sendMsgToOthers(qMap);

}

void ChatDialog::handleHeartbeat(quint32 port) {
	qDebug() << "get heartbeat";
	if (port != leaderPort) {
		updateLeader(port);
	}

	// reset timeout
	timeoutTimer->start(1500 + genRandNum() % 2000);

}

// send msg to all the nodes
void ChatDialog::sendMsgToAll(const QVariantMap &qMap) {
	QVariantMap msg = qMap;
	for (int p = udpSocket->myPortMin; p <= udpSocket->myPortMax; p++) {
		udpSocket->sendUdpDatagram(msg, p);
		//if (p != udpSocket->myPort) {
		//	udpSocket->sendUdpDatagram(qMap, p);
		//}
	}

}

// send msg to all other nodes
void ChatDialog::sendMsgToOthers(const QVariantMap &qMap) {
	QVariantMap msg = qMap;
	for (int p = udpSocket->myPortMin; p <= udpSocket->myPortMax; p++) {
		if (p != udpSocket->myPort) {
			qDebug() << "sending " << msg << " to " << QString::number(p);
			udpSocket->sendUdpDatagram(msg, p);
		}
	}

}

void ChatDialog::resendMsgs() {
	QStringList unsendMsgsTmp;
	for (int i = 0; i < unsendMsgs.size(); i++) {
		QVariantMap newMsg;
		newMsg["Origin"] = udpSocket->originName;
		newMsg["ChatText"] = unsendMsgs[i];
		if (!startRaft) {
			unsendMsgsTmp.append(newMsg["ChatText"].toString());
		} else {
			proposeMsg(newMsg);
		}
	}
	unsendMsgs = unsendMsgsTmp;
}

void ChatDialog::requestAllMsg() {
	QVariantMap qMap;
	
	qMap["Type"] = QString::fromStdString("RequestAllMsg");
	qMap["Origin"] = QString::number(udpSocket->myPort);
	sendMsgToOthers(qMap);
}


void ChatDialog::handleRequestAllMsg(quint32 port) {
	//send all msg only if is leader
	//if (myStates() == QString::fromStdString("LEADER")) {
	sendAllMsg(port);
	//}
}



void ChatDialog::sendAllMsg(quint32 port) {
	qDebug() << "in sendAllMsg";
	QMap<QString, QMap<quint32, QVariantMap> > qMap;
	qMap["Type"][0]["Type"] = QString::fromStdString("AllMsgs");
	qMap["Committed"] = committedMsgs;
	qMap["Uncommitted"] = uncommittedMsgs;
	qMap["Leader"][0]["Leader"] = QString::number(leaderPort);
	
	qDebug() << QString::number(port);
	udpSocket->sendUdpDatagram(qMap, port);

}


void ChatDialog::handleAllMsg(const QMap<QString, QMap<quint32, QVariantMap> >&qMap){
	quint32 leaderp = qMap["Leader"][0]["Leader"].toInt();
	if (leaderp != leaderPort) updateLeader(leaderp);
	
	QMap<quint32, QVariantMap> committedMsgsTmp = qMap["Committed"];
	qDebug() << "!!!!!!!!!!!committedMsgsTmp" << committedMsgsTmp;
	QMap<quint32, QVariantMap> uncommittedMsgsTmp = qMap["Uncommitted"];
	for (QMap<quint32, QVariantMap>::const_iterator iter = committedMsgsTmp.begin(); iter != committedMsgsTmp.end(); ++iter) {
		quint32 seqNoTmp = iter.value()["SeqNo"].toInt();
		if (seqNoTmp >= nextSeqNo) {
			nextSeqNo = seqNoTmp + 1;
		}
		if (!committedMsgs.contains(seqNoTmp)) {
			committedMsgs.insert(seqNoTmp, iter.value());
		}
		if (uncommittedMsgs.contains(seqNoTmp)) {
			uncommittedMsgs.remove(seqNoTmp);
		}
	}
	for (QMap<quint32, QVariantMap>::const_iterator iter = uncommittedMsgsTmp.begin(); iter != uncommittedMsgsTmp.end(); ++iter) {
		quint32 seqNoTmp = iter.value()["SeqNo"].toInt();

		if (seqNoTmp >= nextSeqNo) {
			nextSeqNo = seqNoTmp + 1;
		}
		if (!uncommittedMsgs.contains(seqNoTmp) and !committedMsgs.contains(seqNoTmp)) {
			uncommittedMsgsTmp.insert(seqNoTmp, uncommittedMsgsTmp[seqNoTmp]);
		}
	}


}

void ChatDialog::restoreTimeoutHandler() {
	if (startRaft) {
		this->textview->append("--------------------");
		this->textview->append("----Restored MSG----");
		this->textview->append("--------------------");
		for (QMap<quint32, QVariantMap>::const_iterator iter = committedMsgs.begin(); iter != committedMsgs.end(); ++iter) {
			this->textview->append(iter.value()["Origin"].toString() + ">: " + iter.value()["ChatText"].toString());
		}
	}
	restoreWaitingTimer->stop();
}



void ChatDialog::updateLeader(quint32 port) {
	for (int i = udpSocket->myPortMin; i <= udpSocket->myPortMax; i++) {
		nodeStates[i] = QString::fromStdString("FOLLOWER");
	}
	nodeStates[port] = "LEADER";
	leaderPort = port;
}


QString ChatDialog::myStates(){
	return nodeStates[udpSocket->myPort];
}

int ChatDialog::genRandNum() {
    QDateTime current = QDateTime::currentDateTime();
    uint msecs = current.toTime_t();
    qsrand(msecs);
    return qrand();
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
