#ifndef FLOYD_H_
#define FLOYD_H_

#include <string>

#include "slash/include/slash_mutex.h"
#include "slash/include/slash_status.h"
#include "pink/include/bg_thread.h"

#include "floyd/include/floyd_options.h"
//#include "floyd/src/raft/log.h"
//#include "floyd/src/floyd_context.h"
//#include "floyd/src/floyd_worker.h"

#include "nemo-rocksdb/db_nemo_impl.h"

namespace floyd {
using slash::Status;

class Log;
class RpcClient;
class FloydContext;
class FloydApply;
class FloydWorker;
class FloydWorkerConn;
struct LeaderElectTimerEnv;

namespace command {
class Command;
class CommandRes;
}

class PeerThread;

typedef std::map<std::string, PeerThread*> PeersSet;

class Floyd {
 public:
  Floyd(const Options& options);
  virtual ~Floyd();

  Status Start();

  Status Write(const std::string& key, const std::string& value);
  Status Delete(const std::string& key);
  Status Read(const std::string& key, std::string& value);
  //Status ReadAll(std::map<std::string, std::string>& kvMap);
  Status DirtyRead(const std::string& key, std::string& value);
  //Status DirtyReadAll(std::map<std::string, std::string>& kvMap);
  //Status DirtyWrite(const std::string& key, const std::string& value);
  //Status TryLock(const std::string& key);
  //Status UnLock(const std::string& key);

  // return true if leader has been elected
  bool GetLeader(std::string& ip_port);
  // bool GetServerStatus(std::string& msg);
  
  void BeginLeaderShip();
  RpcClient* peer_rpc_client() {
    return peer_rpc_client_;
  }

 private:
  friend class FloydWorkerConn;
  const Options options_;
  rocksdb::DBNemo* db_;
  Log* log_;
  FloydContext* context_;

  FloydWorker* worker_;
  FloydApply* apply_;
  pink::Timer* leader_elect_timer_;
  LeaderElectTimerEnv* leader_elect_env_;
  PeersSet peers_;
  RpcClient* peer_rpc_client_;

  bool IsSelf(const std::string& ip_port);
  bool IsLeader();
  bool HasLeader();

  uint64_t QuorumMatchIndex();

  Status DoCommand(const command::Command& cmd,
      command::CommandRes *cmd_res);
  Status ExecuteCommand(const command::Command& cmd,
      command::CommandRes *cmd_res);
  void DoRequestVote(command::Command& cmd,
      command::CommandRes* cmd_res);
  void DoAppendEntry(command::Command& cmd,
      command::CommandRes* cmd_res);
  static void StartNewElection(void* arg);
};

} // namespace floyd
#endif