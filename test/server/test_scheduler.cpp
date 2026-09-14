#include "globals.h"
#include "core/util.h"
#include "core/c-wrapper.h"
#include "client/client.h"
#include "client/clientplayer.h"
#include "server/server.h"
#include "server/room/room.h"
#include "server/gamelogic/roomthread.h"
#include "server/user/serverplayer.h"
#include "network/router.h"

/*
  对房间调度模块的测试。需要测试的功能有这些：
  - 房间的启动
  - 房间的游戏结束
  - 房间的切出
  - 异步处理
*/
class TestScheduler: public QObject {
  Q_OBJECT
private slots:
  void initTestCase();
  void testStartGame();
  void testReconnect();
  void testObserve();
  void cleanupTestCase();
};

void TestScheduler::initTestCase() {
  SetupServerAndClient();
  auto client = clients[0], client2 = clients[1], client3 = clients[2];
  client->connectToHostAndSendSetup("localhost", test_port);
  client2->connectToHostAndSendSetup("localhost", test_port);
  client3->connectToHostAndSendSetup("localhost", test_port);
  QSignalSpy spy2(client->getRouter(), &Router::notification_got);
  while (spy2.wait(50))
    qApp->processEvents();
}

void TestScheduler::testStartGame() {
  auto client = clients[0], client2 = clients[1], client3 = clients[2];
  QSignalSpy spy(client->getRouter(), &Router::notification_got);
  QSignalSpy spy2(client2->getRouter(), &Router::notification_got);
  QSignalSpy spy3(client3->getRouter(), &Router::notification_got);

  client->notifyServer("CreateRoom", QVariantList({
    QStringLiteral("test_room2"), 2, 90, room_config.toVariantMap(),
  }));

  // 直接等到spy3收到数据（大厅人数变动） c2加入房间一样的等待
  QVERIFY(spy3.wait());

  client2->notifyServer("EnterRoom", QVariantList({1, ""}));
  QVERIFY(spy3.wait());

  auto room = ServerInstance->findRoom(1);
  auto thread = qobject_cast<RoomThread *>(room->parent());
  // pushRequest由服务端线程发出，QSignalSpy跨线程捕捉信号不可靠（会偶发漏信号），
  // 改用queued连接的lambda接收
  QString pushed_req;
  connect(thread, &RoomThread::pushRequest, this,
          [&pushed_req](const QString &req) { pushed_req = req; });

  // 下一步c1发出StartGame命令
  client->notifyServer("StartGame", "");
  QTRY_VERIFY(!pushed_req.isEmpty());
  QCOMPARE(pushed_req, QStringLiteral("-1,1,newroom"));
  QVERIFY(room->isStarted());

  // 关于startGame后续的测试...
}

void TestScheduler::testReconnect() {
  // 先踢了再说 强制掉线
  auto client = clients[0], client2 = clients[1], client3 = clients[2];
  QSignalSpy spy_disconnet(client2, &Client::error_message);
  QVariantList args;
  auto splayer2 = ServerInstance->findPlayer(client2->getSelf()->getId());
  emit splayer2->kicked();
  QVERIFY(spy_disconnet.wait());
  QCOMPARE(splayer2->getState(), Player::Offline);

  // 再尝试重连
  delete client2;
  client2 = new TesterClient(test_name2, "1234");
  clients[1] = client2;
  QSignalSpy spy2(client2->getRouter(), &Router::notification_got);
  client2->connectToHost("localhost", test_port);
  QVERIFY(spy2.wait()); qApp->processEvents(); // 收NetworkDelayTest 发第一个包
  // 然后应该是以下：
  // Setup, SetServerSettings, Reconnect, RoomOwner (AddSkill系列不管了)
  // 其中一直wait直到收到RoomOwner包只是为了确保client执行了Lua
  spy2.clear();
  while (spy2.count() < 4) QVERIFY(spy2.wait(1000));
  args = spy2[0];
  QCOMPARE(args[0].toString(), "Setup");
  auto setup_data = QCborValue::fromCbor(args[1].toByteArray()).toArray();
  // 格式应该是 [id，用户名，头像，延迟] 只检查是不是设置延迟了（一定要有）
  QCOMPARE(setup_data.size(), 4);
  args = spy2[2];
  QCOMPARE(args[0].toString(), "Reconnect");
}

void TestScheduler::testObserve() {
  auto client = clients[0], client2 = clients[1], client3 = clients[2];
  QSignalSpy spy3(client3->getRouter(), &Router::notification_got);
  client3->notifyServer("ObserveRoom", QVariantList({1, ""}));
  QVERIFY(spy3.wait());
  qApp->processEvents();
}

void TestScheduler::cleanupTestCase() {
  // server_thread->kickAllClients();
}

QTEST_GUILESS_MAIN(TestScheduler)
#include "test_scheduler.moc"
