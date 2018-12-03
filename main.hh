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

private:
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

private:
	QTextEdit *textview;
	QLineEdit *textline;
	NetSocket *udpSocket;
	QTimer *timeoutTimer;
	QTimer *heartbeatTimer;

	QString state;
	quint32 nextSeqNo;
	quint32 nextSeqToShow;

	bool startRaft;

	quint32 leaderPort;

	QStringList myLeaderVote;

	QMap<quint32, QStringList> msgCommits;
	QMap<quint32, bool> declineNodes;
	QMap<quint32, QString> nodeStates;
	QMap<quint32, QVariantMap> commitedMsgs;
	QMap<quint32, QVariantMap> uncommitedMsgs;

	void addToUncommitedMsgs(QVariantMap &qMap);
	void addToCommitedMsgs(QVariantMap &qMap);

	void proposeMsg(QVariantMap &qMap);
	void handleProposeMsg(QVariantMap &qMap);
	void approveMsg(QVariantMap &qMap);
	void handleApproveMsg(QVariantMap &qMap);
	void commitMsg(QVariantMap &qMap);
	void handleCommitMsg(QVariantMap &qMap);

	void proposeLeader();
	void handleProposeLeader(qint32 port);
	void approveLeader(qint32 port);
	void handleApproveLeader(qint32 port);
	void commitLeader();
	void handleCommitLeader(qint32 port);

	void sendAllMsg(qint32 port);
	void handleAllMsg(QMap<QString, QMap<quint32, QVariantMap> >&qMap);

	void sendHeartbeat();
	void handleHeartbeat(qint32 port);
};


#endif // P2PAPP_MAIN_HH
