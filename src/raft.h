#ifndef __FLOYD_CONSENSUS_RAFT_H__
#define __FLOYD_CONSENSUS_RAFT_H__

#include "bg_thread.h"
#include "pink_thread.h"
#include "floyd_util.h"
#include "floyd_mutex.h"
#include "floyd_meta.h"
#include "command.pb.h"
#include "log.h"
#include "state_machine.h"
#include "peer_thread.h"

namespace floyd {

class RaftConsensus {
public:
	enum class Result {
		SUCCESS,
		NOT_LEADER,
    TIMEOUT,
	};

	explicit RaftConsensus(const floyd::Options& options);
	~RaftConsensus();

	void Init();
	void InitAsLeader();

	std::pair<Result, uint64_t> Replicate(const command::Command& cmd);
	void AddNewPeer(NodeInfo* ni);

	floyd::Status HandleWriteCommand(command::Command& cmd);
  floyd::Status HandleReadCommand(command::Command& cmd,std::string& value);
  floyd::Status HandleReadAllCommand(command::Command& cmd,KVMap& kvMap);
  floyd::Status HandleTryLockCommand(command::Command& cmd);
  floyd::Status HandleUnLockCommand(command::Command& cmd);
  floyd::Status HandleDeleteUserCommand(command::Command& cmd);

	void HandleRequestVote(command::Command& cmd, command::CommandRes* cmd_res);
	void HandleAppendEntries(command::Command& cmd, command::CommandRes* cmd_res);
	StateMachine::Entry GetNextCommitEntry(uint64_t index);
	StateMachine::Entry TryGetNextCommitEntry(uint64_t index);
	std::pair<std::string, int> GetLeaderNode();
  void SetVoteCommitIndex(int target_index);
  void SetVoteTerm(int target_term);
  int GetCommitIndex();
  int GetCurrentTerm();
  void WakeUpAll();
  void Wait();

private:
	class PeerThread;

public:
  struct DeleteUserArg {
    RaftConsensus *raft;
    std::string ip;
    int port;
    //command::Command cmd;

    DeleteUserArg(RaftConsensus *_r, const std::string &_ip, int _port)
        : raft(_r), ip(_ip), port(_port) {}
  };
  
	typedef std::function<bool(PeerThread&)> Predicate;
	typedef std::function<void(PeerThread&)> SideEffect;
	typedef std::function<uint64_t(PeerThread&)> GetValue;

	friend class LeaderDiskThread;
	friend class ElectLeaderThread;
	friend class PeerThread;

private:
	Options options_;

	enum class State {
		FOLLOWER,
		CANDIDATE,
		LEADER,
	};
	State state_;

  bool exiting_;
  void Exit();

	class LeaderDiskThread : public pink::Thread {
	public:
		LeaderDiskThread(RaftConsensus* raft_con);
		~LeaderDiskThread();

		virtual void* ThreadMain();

	private:
		RaftConsensus* raft_con_;
	};
	LeaderDiskThread* leader_disk_;

	class ElectLeaderThread : public pink::Thread {
	public:
		ElectLeaderThread(RaftConsensus* raft_con);
		~ElectLeaderThread();

		virtual void* ThreadMain();
		void StartNewElection();

	private:
		RaftConsensus* raft_con_;
	};
	ElectLeaderThread* elect_leader_;
	struct timespec start_election_at_;
	std::string leader_ip_;
	int leader_port_;
	std::string voted_for_ip_;
	int voted_for_port_;
	uint64_t last_synced_index_;
	void BecomeLeader();

  bool voteable_;
	uint64_t vote_target_term_;
	uint64_t vote_target_index_;


	std::vector<PeerThread*> peers_;
	struct timespec period_;
	void Append(std::vector<Log::Entry*>& entries);
	uint64_t GetLastLogTerm();
	void ForEach(const SideEffect& sideEffect);
	void SetElectionTimer();
	void InterruptAll();
	uint64_t QuorumMin(const GetValue& getvalue);
	bool QuorumAll(const Predicate& predicate);
	void UpdateLogMetadata();
  void AdvanceCommitIndex();
  std::pair<RaftConsensus::Result, uint64_t> WaitForCommitIndex(uint64_t index);
	void StepDown(uint64_t new_term);
  void InitPeerThreads();

	// which protect raftconsus
	mutable Mutex mutex_;
	mutable CondVar state_changed_;
	
	std::unique_ptr<Log> log_;
	bool log_sync_queued_;
	
	uint64_t current_term_;
	uint64_t commit_index_;

  pink::BGThread bg_thread_; 

	StateMachine* sm_;
};

}

#endif
