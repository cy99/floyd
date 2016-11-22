#ifndef INCLUDE_FLOYD_META_H_
#define INCLUDE_FLOYD_META_H_

#include "holy_thread.h"
#include "pb_conn.h"
#include "pb_cli.h"
#include "meta.pb.h"
#include "floyd_define.h"
#include "floyd_util.h"
#include "floyd_worker.h"
#include "status.h"
#include "slice.h"

namespace floyd {

class NodeInfo;
class FloydMetaCliConn;
typedef std::pair<std::string, int> NodeAddr;

struct NodeInfo {
  std::string ip;
  int port;
  time_t last_ping;
  NodeStatus ns;
  FloydMetaCliConn* mcc;
  FloydWorkerCliConn* dcc;

  NodeInfo(const std::string& ip, const int port);

  Status UpHoldWorkerCliConn(bool create_new_connect = false);
  bool operator==(NodeInfo& node_y);
};

class FloydMetaCliConn : public pink::PbCli {
 public:
  FloydMetaCliConn(const std::string& ip, const int port);
  virtual ~FloydMetaCliConn();
  virtual Status Connect();
  virtual Status GetResMessage(meta::MetaRes* meta_res);
  virtual Status SendMessage(meta::Meta* meta);
  Status GetNodeInfo(std::vector<NodeInfo*>* nis);

 private:
  std::string local_ip_;
  int local_port_;
};

class FloydMetaConn : public pink::PbConn {
 public:
  FloydMetaConn(int fd, std::string& ip_port);
  FloydMetaConn(int fd, std::string& ip_port, pink::Thread* thread);
  virtual ~FloydMetaConn();

  virtual int DealMessage();

 private:
  meta::Meta meta_;
  meta::MetaRes meta_res_;
};

class FloydMetaThread : public pink::HolyThread<FloydMetaConn> {
 public:
  explicit FloydMetaThread(int port);
  virtual ~FloydMetaThread();
};

}  // namespace floyd
#endif  // INCLUDE_FLOYD_META_H_
