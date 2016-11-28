#include "status.h"
#include "raft.h"
#include "floyd.h"
#include "log.h"
#include "memory_log.h"
#include "simplefile_log.h"
#include "file_log.h"
#include "floyd_define.h"
#include "logger.h"

namespace floyd {

RaftConsensus::RaftConsensus(const floyd::Options& options)
    : options_(options),
      state_(State::FOLLOWER),
      exiting_(false),
      leader_ip_(""),
      leader_port_(0),
      voted_for_ip_(""),
      voted_for_port_(0),
      last_synced_index_(0),
      voteable_(false),
      vote_target_term_(std::numeric_limits<int>::max()),
      vote_target_index_(std::numeric_limits<int>::max()),
      period_((struct timespec) {0, 200000000}),
      state_changed_(&mutex_),
      log_(),
      log_sync_queued_(false),
      current_term_(0),
      commit_index_(0) {
  leader_disk_ = new LeaderDiskThread(this);
  elect_leader_ = new ElectLeaderThread(this);
  start_election_at_.tv_sec = std::numeric_limits<time_t>::max();
  start_election_at_.tv_nsec = 0;
  sm_ = new StateMachine(this);
}

RaftConsensus::~RaftConsensus() {
  if (!exiting_) Exit();

  leader_disk_->JoinThread();
  delete leader_disk_;
  elect_leader_->JoinThread();
  delete elect_leader_;
  std::vector<PeerThread*>::iterator iter = peers_.begin();
  for (; iter != peers_.end(); ++iter) {
    (*iter)->JoinThread();
    delete *iter;
  }
  delete sm_;

  if (log_sync_queued_) {
    std::unique_ptr<Log::Sync> sync = log_->TakeSync();
    sync->Wait();
  }
}

void RaftConsensus::SetVoteCommitIndex(int target_index) {
  MutexLock l(&mutex_);
  vote_target_index_ = target_index;
}

void RaftConsensus::SetVoteTerm(int target_term) {
  MutexLock l(&mutex_);
  vote_target_term_ = target_term;
}
int RaftConsensus::GetCommitIndex() {
  MutexLock l(&mutex_);
  return commit_index_;
}
int RaftConsensus::GetCurrentTerm() {
  MutexLock l(&mutex_);
  return current_term_;
}
void RaftConsensus::Init() {
  if (!log_) {
    if (options_.log_type == "MemoryLog") {
      log_.reset(new MemoryLog());
    } else if (options_.log_type == "SimpleFileLog") {
      log_.reset(new SimpleFileLog(options_.log_path));
    } else if (options_.log_type == "FileLog") {
      log_.reset(new FileLog(options_.log_path));
    } else {
      assert(false);
    }
  }

  //==================
  // Test FileLog
  // ================
  // std::vector<Log::Entry*> entries;
  // for (int i = 0; i < 5; i++) {
  //   floyd::raft::Entry entry;
  // 	entry.set_type(floyd::raft::Entry::DATA);
  // 	entry.set_term(current_term_);

  //   command::Command cmd;
  //   cmd.set_type(command::Command::Write);
  //   command::Command_Kv* kv = new command::Command_Kv();
  //   kv->set_key("key" + i);
  //   kv->set_value("value");
  //   cmd.set_allocated_kv(kv);

  // 	uint64_t len = cmd.ByteSize();
  // 	char* data = new char[len + 1];
  // 	cmd.SerializeToArray(data, len);
  // 	entry.set_cmd(data, len);

  //   entries.push_back(&entry);
  // }
  // log_->Append(entries);
  //
  // log_->TruncateSuffix(2);
  //

  if (log_->metadata.has_current_term())
    current_term_ = log_->metadata.current_term();
  if (log_->metadata.has_voted_for_ip() &&
      log_->metadata.has_voted_for_port()) {
    voted_for_ip_ = log_->metadata.voted_for_ip();
    voted_for_port_ = log_->metadata.voted_for_port();
  }

  StepDown(current_term_);

  // Start threads
  leader_disk_->StartThread();
  elect_leader_->StartThread();
  uint64_t next_index = log_->GetLastLogIndex() + 1;
  InitPeerThreads();
  std::vector<PeerThread*>::iterator iter = peers_.begin();
  for (; iter != peers_.end(); ++iter) {
    (*iter)->set_next_index(next_index);
    (*iter)->StartThread();
  }
  sm_->Init();
}

void RaftConsensus::InitAsLeader() {
  if (!log_) {
    if (options_.log_type == "MemoryLog") {
      log_.reset(new MemoryLog());
    } else if (options_.log_type == "SimpleFileLog") {
      log_.reset(new SimpleFileLog(options_.log_path));
    } else if (options_.log_type == "FileLog") {
      log_.reset(new FileLog(options_.log_path));
    } else {
      assert(false);
    }
  }

  if (log_->metadata.has_current_term())
    current_term_ = log_->metadata.current_term();
  if (log_->metadata.has_voted_for_ip() &&
      log_->metadata.has_voted_for_port()) {
    voted_for_ip_ = log_->metadata.voted_for_ip();
    voted_for_port_ = log_->metadata.voted_for_port();
  }

  StepDown(current_term_);

  // Start threads
  leader_disk_->StartThread();
  elect_leader_->StartThread();
  uint64_t next_index = log_->GetLastLogIndex() + 1;
  sm_->Init();
  state_ = State::CANDIDATE;
  BecomeLeader();
}
void RaftConsensus::InitPeerThreads() {
  MutexLock l(&floyd::Floyd::nodes_mutex);
  std::vector<NodeInfo*>::iterator iter = floyd::Floyd::nodes_info.begin();
  for (; iter != floyd::Floyd::nodes_info.end(); ++iter) {
    if ((*iter)->ip != options_.local_ip ||
        (*iter)->port != options_.local_port) {
      PeerThread* pt = new PeerThread(this, *iter);
      peers_.push_back(pt);
    }
  }
}
void RaftConsensus::Exit() {
  MutexLock l(&mutex_);
  exiting_ = true;
  state_changed_.SignalAll();
}

void RaftConsensus::Append(std::vector<Log::Entry*>& entries) {
  // printf ("log append entry\n");
  log_->Append(entries);
  if (state_ == State::LEADER) {
    log_sync_queued_ = true;
  } else {
    std::unique_ptr<Log::Sync> sync = log_->TakeSync();
    sync->Wait();
    log_->SyncComplete(std::move(sync));
  }

  state_changed_.SignalAll();
}

void RaftConsensus::AddNewPeer(NodeInfo* ni) {
  MutexLock l(&mutex_);
  PeerThread* pt = new PeerThread(this, ni);
  peers_.push_back(pt);
  pt->StartThread();
}

std::pair<std::string, int> RaftConsensus::GetLeaderNode() {
  MutexLock l(&mutex_);
  return {leader_ip_, leader_port_};
}

std::pair<RaftConsensus::Result, uint64_t> RaftConsensus::Replicate(
    const command::Command& cmd) {
  MutexLock l(&mutex_);
  if (state_ == State::LEADER) {
    // printf("replicat cmd to peers\n");
    std::vector<Log::Entry*> entries;
    Log::Entry entry;
    uint64_t len = cmd.ByteSize();
    char* data = new char[len + 1];
    cmd.SerializeToArray(data, len);
    entry.set_type(floyd::raft::Entry::DATA);
    entry.set_term(current_term_);
    entry.set_cmd(data, len);
    delete data;
    entries.push_back(&entry);
    Append(entries);

    uint64_t index = log_->GetLastLogIndex();
    return {Result::SUCCESS, index};
  }

  return {Result::NOT_LEADER, 0};
}

std::pair<RaftConsensus::Result, uint64_t> RaftConsensus::WaitForCommitIndex(
    uint64_t index) {
  MutexLock l(&mutex_);
  if (peers_.empty()) {
    // AdvanceCommitIndex();
    commit_index_ = index;
    state_changed_.SignalAll();
    return {Result::SUCCESS, index};
  } else {
    //@TODO ADD TIMEOUT
    struct timeval now;
    struct timespec timeout_at;
    gettimeofday(&now, NULL);
    timeout_at.tv_sec = now.tv_sec + 10;
    timeout_at.tv_nsec = now.tv_usec * 1000;
    while (true) {
      if (commit_index_ >= index) {
        LOG_DEBUG("MainThread::WaitForCommitIndex: Waked up by PeerThread");
        return {Result::SUCCESS, index};
      }
      gettimeofday(&now, NULL);
      if ((now.tv_sec > timeout_at.tv_sec) ||
          ((now.tv_sec == timeout_at.tv_sec) &&
           (now.tv_usec * 1000 > timeout_at.tv_nsec)))
        return {Result::TIMEOUT, index};
      state_changed_.WaitUntil(timeout_at);
    }
  }
}

uint64_t RaftConsensus::GetLastLogTerm() {
  uint64_t last_log_index = log_->GetLastLogIndex();
  if (last_log_index == 0) return 0;
  return log_->GetEntry(last_log_index).term();
}

void RaftConsensus::ForEach(const SideEffect& side_effect) {
  for (auto it = peers_.begin(); it != peers_.end(); ++it) side_effect(**it);
}

void RaftConsensus::SetElectionTimer() {
  struct timeval seed;
  gettimeofday(&seed, NULL);
  srand(seed.tv_usec);
  double rd = ((double)rand() / RAND_MAX) * 3;
  uint64_t ms =
      (uint64_t)(options_.elect_timeout_ms + rd * options_.elect_timeout_ms);
  struct timeval now;
  gettimeofday(&now, NULL);
  start_election_at_.tv_sec = now.tv_sec + ms / 1000;
  start_election_at_.tv_nsec = now.tv_usec * 1000 + (ms % 1000) * 1000000;
  start_election_at_.tv_sec += start_election_at_.tv_nsec / 1000000000;
  start_election_at_.tv_nsec %= 1000000000;
  state_changed_.SignalAll();
}

void RaftConsensus::UpdateLogMetadata() {
  log_->metadata.set_current_term(current_term_);
  log_->metadata.set_voted_for_ip(voted_for_ip_);
  log_->metadata.set_voted_for_port(voted_for_port_);
  log_->UpdateMetadata();
}

void RaftConsensus::WakeUpAll() {
  MutexLock l(&mutex_);
  state_changed_.SignalAll();
}
void RaftConsensus::Wait() {
  MutexLock l(&mutex_);
  state_changed_.Wait();
}

void RaftConsensus::AdvanceCommitIndex() {
  if (state_ != State::LEADER) return;

  uint64_t new_commit_index = QuorumMin(&PeerThread::GetLastAgreeIndex);
  if (commit_index_ >= new_commit_index) {
    if (commit_index_ > sm_->last_apply_index()) {
      state_changed_.SignalAll();
    }
    return;
  }
  if (log_->GetEntry(new_commit_index).term() != current_term_) return;
  commit_index_ = new_commit_index;
  LOG_DEBUG("AdvanceCommitIndex: commit_index_ = %d", commit_index_);
  state_changed_.SignalAll();
}

StateMachine::Entry RaftConsensus::GetNextCommitEntry(uint64_t index) {
  MutexLock l(&mutex_);
  uint64_t next_index = index + 1;
  while (true) {
    if (commit_index_ >= next_index) {
      StateMachine::Entry entry;
      const Log::Entry& log_entry = log_->GetEntry(next_index);
      entry.index = next_index;
      entry.log_entry = log_entry;
      return entry;
    }
    state_changed_.Wait();
  }
}

StateMachine::Entry RaftConsensus::TryGetNextCommitEntry(uint64_t index) {
  MutexLock l(&mutex_);
  uint64_t next_index = index + 1;
  StateMachine::Entry entry;
  if (commit_index_ >= next_index) {
    const Log::Entry& log_entry = log_->GetEntry(next_index);
    entry.index = next_index;
    entry.log_entry = log_entry;
  } else {
    entry.index = index;
  }
  return entry;
}

void RaftConsensus::BecomeLeader() {
  assert(state_ == State::CANDIDATE);
  state_ = State::LEADER;
  leader_ip_ = options_.local_ip;
  leader_port_ = options_.local_port;
  start_election_at_.tv_sec = std::numeric_limits<time_t>::max();
  start_election_at_.tv_nsec = 0;

  ForEach(&PeerThread::BeginLeaderShip);
  // printf ("I am become Leader\n");

  // Append noop entry to guarantee that new leader can
  // Get commitindex timely.
  std::vector<Log::Entry*> entries;
  Log::Entry entry;
  entry.set_type(floyd::raft::Entry::NOOP);
  entry.set_term(current_term_);
  entries.push_back(&entry);
  Append(entries);

  state_changed_.SignalAll();
}

void RaftConsensus::InterruptAll() { state_changed_.SignalAll(); }

uint64_t RaftConsensus::QuorumMin(const GetValue& getvalue) {
  if (peers_.empty()) return last_synced_index_;
  std::vector<uint64_t> values;
  std::vector<PeerThread*>::iterator iter = peers_.begin();
  for (; iter != peers_.end(); ++iter) {
    values.push_back(getvalue(**iter));
  }
  std::sort(values.begin(), values.end());
  return values.at(values.size() / 2);
}

bool RaftConsensus::QuorumAll(const Predicate& predicate) {
  if (peers_.empty()) return true;
  uint64_t count = 1;
  std::vector<PeerThread*>::iterator iter = peers_.begin();
  for (; iter != peers_.end(); ++iter) {
    if (predicate(**iter)) ++count;
  }
  return (count >= (peers_.size() + 1) / 2 + 1);
}

void RaftConsensus::StepDown(uint64_t new_term) {
  if (state_ == State::LEADER) {
  } else if (state_ == State::CANDIDATE) {
  } else if (state_ == State::FOLLOWER) {
  }

  if (current_term_ < new_term) {
    current_term_ = new_term;
    leader_ip_ = "";
    leader_port_ = 0;
    voted_for_ip_ = "";
    voted_for_port_ = 0;
    UpdateLogMetadata();
    state_ = State::FOLLOWER;
  } else {
    state_ = State::FOLLOWER;
  }

  if (start_election_at_.tv_sec == std::numeric_limits<time_t>::max())
    SetElectionTimer();

  if (log_sync_queued_) {
    std::unique_ptr<Log::Sync> sync = log_->TakeSync();
    sync->Wait();
    log_sync_queued_ = false;
  }
}

floyd::Status RaftConsensus::HandleWriteCommand(command::Command& cmd) {
  std::pair<Result, uint64_t> result = Replicate(cmd);
  if (result.first == Result::NOT_LEADER)
    return floyd::Status::NotFound("no leader!");
  result = WaitForCommitIndex(result.second);
  if (result.first == Result::TIMEOUT)
    return floyd::Status::NotFound("write commit timeout");
  uint64_t log_index = result.second;
  if (!sm_->WaitForWriteResponse(log_index))
    return floyd::Status::Corruption("exec command error!");
  return floyd::Status::OK();
}

floyd::Status RaftConsensus::HandleReadCommand(command::Command& cmd,
                                               std::string& value) {
  LOG_DEBUG("MainThread::HandleReadCommand start");
  command::Command_Kv kv = cmd.kv();
  std::string key = kv.key();
  std::pair<Result, uint64_t> result = Replicate(cmd);
  LOG_DEBUG(
      "MainThread::HandleReadCommand: step1(Replicate) done, ret: <%d, %d>",
      result.first, result.second);
  if (result.first == Result::NOT_LEADER)
    return floyd::Status::NotFound("no leader!");
  result = WaitForCommitIndex(result.second);
  LOG_DEBUG(
      "MainThread::HandleReadCommand: step2(WaitForCommitIndex) done, ret: "
      "<%d, %d>",
      result.first, result.second);
  if (result.first == Result::TIMEOUT)
    return floyd::Status::NotFound("read commit timeout");
  uint64_t log_index = result.second;
  if (!sm_->WaitForReadResponse(log_index, key, value)) {
    LOG_DEBUG(
        "MainThread::HandleReadCommand: step3(WaitForReadResponse) done, ret: "
        "false");
    LOG_DEBUG("MainThread::HandleReadCommand end");
    return floyd::Status::Corruption("exec command error!");
  }
  LOG_DEBUG(
      "MainThread::HandleReadCommand: step3(WaitForReadResponse) done, ret: "
      "true");
  LOG_DEBUG("MainThread::HandleReadCommand end");
  return floyd::Status::OK();
}

floyd::Status RaftConsensus::HandleReadAllCommand(command::Command& cmd,
                                                  KVMap& kvMap) {
  std::pair<Result, uint64_t> result = Replicate(cmd);
  if (result.first == Result::NOT_LEADER)
    return floyd::Status::NotFound("no leader!");
  result = WaitForCommitIndex(result.second);
  if (result.first == Result::TIMEOUT)
    return floyd::Status::NotFound("read all commit timeout");
  uint64_t log_index = result.second;
  if (!sm_->WaitForReadAllResponse(log_index, kvMap)) {
    return floyd::Status::Corruption("exec command error!");
  }
  return floyd::Status::OK();
}

floyd::Status RaftConsensus::HandleTryLockCommand(command::Command& cmd) {

  LOG_DEBUG("MainThread::HandleTryLockCommand start");
  std::string key = cmd.kv().key();
  std::pair<Result, uint64_t> result = Replicate(cmd);
  LOG_DEBUG(
      "MainThread::HandleTryLockCommand: step1(Replicate) done, ret: <%d, %d>",
      result.first, result.second);

  if (result.first == Result::NOT_LEADER)
    return floyd::Status::NotFound("no leader!");

  result = WaitForCommitIndex(result.second);
  LOG_DEBUG(
      "MainThread::HandleTryLockCommand: step2(WaitForCommitIndex) done, ret: "
      "<%d, %d>",
      result.first, result.second);
  if (result.first == Result::TIMEOUT)
    return floyd::Status::NotFound("try lock commit timeout");
  uint64_t log_index = result.second;
  Status ret = sm_->WaitForTryLockResponse(log_index);
  LOG_DEBUG(
      "MainThread::HandleTryLockCommand: step3(WaitForTryLockResponse) done, "
      "ret: %s",
      ret.ToString().c_str());
  LOG_DEBUG("MainThread::HandleTryLockCommand end");

  // printf ("handle TryLock result :%s\n", ret.ToString().c_str());
  return ret;
}

floyd::Status RaftConsensus::HandleUnLockCommand(command::Command& cmd) {

  std::string key = cmd.kv().key();
  std::pair<Result, uint64_t> result = Replicate(cmd);

  if (result.first == Result::NOT_LEADER)
    return floyd::Status::NotFound("no leader!");

  result = WaitForCommitIndex(result.second);
  if (result.first == Result::TIMEOUT)
    return floyd::Status::NotFound("unlock commit timeout");
  uint64_t log_index = result.second;
  Status ret = sm_->WaitForUnLockResponse(log_index);

  // printf ("handle UnLock result :%s\n", ret.ToString().c_str());
  return ret;
}

floyd::Status RaftConsensus::HandleDeleteUserCommand(command::Command& cmd) {
  // printf ("\nhandle DeleteUser cmd\n");

  std::pair<Result, uint64_t> result = Replicate(cmd);

  if (result.first == Result::NOT_LEADER) {
    return floyd::Status::NotFound("no leader!");
  }

  result = WaitForCommitIndex(result.second);
  if (result.first == Result::TIMEOUT)
    return floyd::Status::NotFound("delete user commit timeout");
  uint64_t log_index = result.second;
  Status ret = sm_->WaitForDeleteUserResponse(log_index);

  // printf ("handle DeleteUser result :%s\n", ret.ToString().c_str());
  return ret;
}

static void HandleDeleteUserWrapper(void* arg) {
  RaftConsensus::DeleteUserArg* d =
      reinterpret_cast<RaftConsensus::DeleteUserArg*>(arg);
  RaftConsensus* raft = d->raft;

  // Construct Cmd PB package
  command::Command cmd;
  cmd.set_type(command::Command::DeleteUser);

  command::Command_User* user = new command::Command_User();
  user->set_ip(d->ip);
  user->set_port(d->port);
  cmd.set_allocated_user(user);

  raft->HandleDeleteUserCommand(cmd);
}

void RaftConsensus::HandleAppendEntries(command::Command& cmd,
                                        command::CommandRes* cmd_res) {
  MutexLock l(&mutex_);
  cmd_res->set_type(command::CommandRes::RaftAppendEntries);
  floyd::raft::AppendEntriesResponse* aers =
      new floyd::raft::AppendEntriesResponse();
  aers->set_status(false);
  aers->set_term(current_term_);
  cmd_res->set_allocated_aers(aers);

  if (cmd.aerq().term() < current_term_) return;
  if (cmd.aerq().term() > current_term_) aers->set_term(cmd.aerq().term());
  StepDown(cmd.aerq().term());
  SetElectionTimer();

  if (leader_ip_ == "" || leader_port_ == 0) {
    leader_ip_ = cmd.aerq().ip();
    leader_port_ = cmd.aerq().port();
  } else {
    assert(leader_ip_ == cmd.aerq().ip() && leader_port_ == cmd.aerq().port());
  }

  if (cmd.aerq().prev_log_index() != 0) {
    if (cmd.aerq().prev_log_index() > log_->GetLastLogIndex()) return;
    if (cmd.aerq().prev_log_term() !=
        log_->GetEntry(cmd.aerq().prev_log_index()).term())
      return;
  }

  aers->set_status(true);

  /* Append the leader's logs, a follower's logs should stale than
   * the log of leader in normal condition, then, the logs of leader
   * append to local sequential
   * But, when leader has changed, local logs may less or more than logs of
   * leader
   * so we need delete the logs that not exist in new leader.
   */
  uint64_t index = cmd.aerq().prev_log_index();
  for (auto it = cmd.aerq().entries().begin(); it != cmd.aerq().entries().end();
       ++it) {
    ++index;
    const floyd::raft::Entry& entry = *it;
    if (log_->GetLastLogIndex() >= index) {
      if (log_->GetEntry(index).term() == entry.term()) continue;
      log_->TruncateSuffix(index - 1);
    }

    std::vector<Log::Entry*> entries;
    do {
      const floyd::raft::Entry& entry = *it;
      entries.push_back(&(const_cast<floyd::raft::Entry&>(entry)));
      ++it;
      ++index;
    } while (it != cmd.aerq().entries().end());
    Append(entries);
    break;
  }

  if (commit_index_ < cmd.aerq().commit_index()) {
    commit_index_ = cmd.aerq().commit_index();
    state_changed_.SignalAll();
  }
}

void RaftConsensus::HandleRequestVote(command::Command& cmd,
                                      command::CommandRes* com_res) {
  MutexLock l(&mutex_);
  uint64_t last_log_index = log_->GetLastLogIndex();
  uint64_t last_log_term = GetLastLogTerm();
  bool can_grant = (cmd.rqv().last_log_term() > last_log_term ||
                    (cmd.rqv().last_log_term() == last_log_term &&
                     cmd.rqv().last_log_index() >= last_log_index));
  bool granted = false;

  if (cmd.rqv().term() > current_term_ && can_grant) {
    StepDown(cmd.rqv().term());
  }

  // a new node is not allowed to vote,until it's term and commitindex is not
  // stale
  if (commit_index_ >= vote_target_index_ &&
      current_term_ >= vote_target_term_) {
    voteable_ = true;
  }
  if (cmd.rqv().term() == current_term_) {
    if (can_grant && voted_for_ip_ == "" && voted_for_port_ == 0) {
      if (voteable_) {
        StepDown(current_term_);
        SetElectionTimer();
        voted_for_ip_ = cmd.rqv().ip();
        voted_for_port_ = cmd.rqv().port();
        UpdateLogMetadata();
        granted = true;
      } else {
        StepDown(current_term_);
        SetElectionTimer();
      }
    } else {
    }
  }

  com_res->set_type(command::CommandRes::RaftVote);
  floyd::raft::ResponseVote* rsv = new floyd::raft::ResponseVote();
  rsv->set_term(current_term_);
  rsv->set_granted(granted);
  com_res->set_allocated_rsv(rsv);
}

/********************LEADER DISK THREAD************************/
RaftConsensus::LeaderDiskThread::LeaderDiskThread(RaftConsensus* raft_con)
    : raft_con_(raft_con) {}

RaftConsensus::LeaderDiskThread::~LeaderDiskThread() {}

void* RaftConsensus::LeaderDiskThread::ThreadMain() {
  MutexLock l(&raft_con_->mutex_);
  while (!raft_con_->exiting_) {
    if (raft_con_->state_ == RaftConsensus::State::LEADER &&
        raft_con_->log_sync_queued_) {
      LOG_DEBUG(
          "LeaderDiskThread::ThreadMain: Waked up by MainThread::Replicate");
      std::unique_ptr<Log::Sync> sync = raft_con_->log_->TakeSync();
      raft_con_->log_sync_queued_ = false;
      {
        MutexUnLock ul(&raft_con_->mutex_);
        sync->Wait();
      }

      raft_con_->last_synced_index_ = sync->last_index;
      raft_con_->AdvanceCommitIndex();
      continue;
    }
    //@TODO right? all conds should be protected by mutex
    raft_con_->state_changed_.Wait();
  }

  return NULL;
}

/**************************************************************/

/********************ELECT LEADER THREAD************************/
RaftConsensus::ElectLeaderThread::ElectLeaderThread(RaftConsensus* raft_con)
    : raft_con_(raft_con) {}

RaftConsensus::ElectLeaderThread::~ElectLeaderThread() {}

void* RaftConsensus::ElectLeaderThread::ThreadMain() {
  struct timeval now;
  MutexLock l(&raft_con_->mutex_);
  while (!raft_con_->exiting_) {
    gettimeofday(&now, NULL);
    if (raft_con_->start_election_at_.tv_sec < now.tv_sec ||
        (raft_con_->start_election_at_.tv_sec == now.tv_sec &&
         raft_con_->start_election_at_.tv_nsec <= now.tv_usec * 1000))
      StartNewElection();
    raft_con_->state_changed_.WaitUntil(raft_con_->start_election_at_);
  }

  return NULL;
}

void RaftConsensus::ElectLeaderThread::StartNewElection() {
  ++raft_con_->current_term_;
  raft_con_->state_ = RaftConsensus::State::CANDIDATE;
  raft_con_->leader_ip_ = "";
  raft_con_->leader_port_ = 0;
  raft_con_->voted_for_ip_ = raft_con_->options_.local_ip;
  raft_con_->voted_for_port_ = raft_con_->options_.local_port;
  raft_con_->SetElectionTimer();
  raft_con_->ForEach(&PeerThread::BeginRequestVote);
  raft_con_->UpdateLogMetadata();
  raft_con_->InterruptAll();

  if (raft_con_->QuorumAll(&PeerThread::HaveVote)) raft_con_->BecomeLeader();
}

}
