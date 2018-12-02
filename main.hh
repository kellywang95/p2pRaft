#ifndef P2PAPP_MAIN_HH
#define P2PAPP_MAIN_HH

#include <QDialog>
#include <QTextEdit>
#include <QLineEdit>
#include <QUdpSocket>
#include <QMutex>
#include <QTimer>

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
	//int getWritePort();
	int resendRumorPort(int port);
	void changeRandomPort();
	void sendUdpDatagram(const QVariantMap &qMap, int port);
	void sendUdpDatagram(const QMap<QString, QVariantMap> &qMap, int port);
	
	
	int myPort;
	int neighborPort;
	int randomPort; //for anti-entropy
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
	void antiEntropyHandler();

private:
	QTextEdit *textview;
	QLineEdit *textline;
	NetSocket *udpSocket;
	QTimer *timeoutTimer;
	QTimer *antiEntropyTimer;
	
	QVariantMap myWants;
	QMap<QString, QMap<quint32, QString> > allMessages;
	QVariantMap pendingMsg;
	QMutex mutex1;  // for myWants
	QMutex mutex2;  // for allMessages
	QMutex mutex3;  // for pendingMsg

	void writeRumorMessage(QString &origin, quint32 seqNo, QString &text, qint32 port, bool addToMsg);
	void writeStatusMessage(int port, QString origin, qint32 seqNo);
	void addToMessages(QVariantMap &qMap);
	void handleStatusMsg(QVariantMap &gotWants, quint16 port);
	void handleRumorMsg(QVariantMap &rumorMap, quint16 port);
};


#endif // P2PAPP_MAIN_HH
