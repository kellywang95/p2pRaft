#ifndef P2PAPP_MAIN_HH
#define P2PAPP_MAIN_HH

#include <QDialog>
#include <QTextEdit>
#include <QLineEdit>
#include <QUdpSocket>
#include <QMutex>
#include <QTimer>
#include <QStringList>
#include <QList>

class NetSocket : public QUdpSocket
{
	Q_OBJECT

public:
	NetSocket(QObject *parent);
	~NetSocket();

    // Bind this socket to a P2Papp-specific default port.
    bool bind();
	// bool readUdp(QVariantMap *map);
	int genRandNum();


	void sendUdpDatagram(const QVariantMap &qMap, int port);
	void sendUdpDatagram(const QMap<QString, QMap<quint32, QVariantMap> >&qMap, int port);
	
	int myPort;

	QHostAddress HostAddress;
	QString originName;

//private:
	int myPortMin, myPortMax;
};

class ChatDialog : public QDialog
{
	Q_OBJECT

public:
	ChatDialog();

public slots:
	void gotReturnPressed();
	void gotReadyRead();
	void timeoutHandler();
	void heartbeatHandler();

private:
	QTextEdit *textview;
	QLineEdit *textline;
	NetSocket *udpSocket;
	QTimer *timeoutTimer;
	QTimer *heartbeatTimer;

	quint32 nextSeqNo;
	quint32 nextSeqToShow;
	
	quint32 voteLeaderRound;

	bool startRaft;

	quint32 leaderPort;

	QStringList VoteToMe;  // lists of nodes that vote me as the leader
	quint32 currentVote;   // vote who as the leader in current round

	QMap<quint32, QStringList> msgApproves;
	QMap<quint32, bool> declineNodes;
	QMap<quint32, QString> nodeStates;

	QMap<quint32, QVariantMap> committedMsgs;
	QMap<quint32, QVariantMap> uncommittedMsgs;

	QStringList unsendMsgs;

	void addToUncommittedMsgs(const QVariantMap &qMap);
	void addToCommittedMsgs(const QVariantMap &qMap);
	void removeFromUncommittedMsgs(const QVariantMap &qMap);

	void proposeMsg(const QVariantMap &qMap);
	void handleProposeMsg(const QVariantMap &qMap);
	void approveMsg(const QVariantMap &qMap);
	void handleApproveMsg(const QVariantMap &qMap);
	void commitMsg(const QVariantMap &qMap);
	void handleCommitMsg(const QVariantMap &qMap);

	void proposeLeader();
	void handleProposeLeader(quint32 port, quint32 round);
	void approveLeader(quint32 port);
	void handleApproveLeader(quint32 port);
	void commitLeader(quint32 port);
	void handleCommitLeader(quint32 port);

	void requestAllMsg();
	void handleRequestAllMsg(quint32 port);
	void sendAllMsg(quint32 port);	
	void handleAllMsg(const QMap<QString, QMap<quint32, QVariantMap> >&qMap);


	void sendMsgToAll(const QVariantMap &qMap);
	void sendMsgToOthers(const QVariantMap &qMap);

	void resendMsgs();

	void sendHeartbeat();
	void handleHeartbeat(quint32 port);

	void updateLeader(quint32 port);
	QString myStates();

	int genRandNum();
};


#endif // P2PAPP_MAIN_HH
