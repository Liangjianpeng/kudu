// Copyright (c) 2013, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.

#ifndef KUDU_CONSENSUS_CONSENSUS_PEERS_H_
#define KUDU_CONSENSUS_CONSENSUS_PEERS_H_

#include <string>
#include <tr1/memory>
#include <vector>

#include "kudu/consensus/consensus.pb.h"
#include "kudu/consensus/ref_counted_replicate.h"
#include "kudu/rpc/response_callback.h"
#include "kudu/rpc/rpc_controller.h"
#include "kudu/server/metadata.pb.h"
#include "kudu/util/countdown_latch.h"
#include "kudu/util/locks.h"
#include "kudu/util/resettable_heartbeater.h"
#include "kudu/util/semaphore.h"
#include "kudu/util/status.h"

namespace kudu {
class HostPort;
class ThreadPool;

namespace log {
class Log;
}

namespace rpc {
class Messenger;
class RpcController;
}

namespace consensus {
class ConsensusServiceProxy;
class OpId;
class PeerProxy;
class PeerProxyFactory;
class PeerMessageQueue;
class VoteRequestPB;
class VoteResponsePB;

// A peer in consensus (local or remote).
//
// Leaders use peers to update the local Log and remote replicas.
//
// Peers are owned by the consensus implementation and do not keep
// state aside from whether there are requests pending or if requests
// are being processed.
//
// There are two external actions that trigger a state change:
//
// SignalRequest(): Called by the consensus implementation, notifies
// that the queue contains messages to be processed.
//
// ProcessResponse() Called a response from a peer is received.
//
// The following state diagrams describe what happens when a state
// changing method is called.
//
//                        +
//                        |
//       SignalRequest()  |
//                        |
//                        |
//                        v
//              +------------------+
//       +------+    processing ?  +-----+
//       |      +------------------+     |
//       |                               |
//       | Yes                           | No
//       |                               |
//       v                               v
//     return                      ProcessNextRequest()
//                                 processing = true
//                                 - get reqs. from queue
//                                 - update peer async
//                                 return
//
//                         +
//                         |
//      ProcessResponse()  |
//      processing = false |
//                         v
//               +------------------+
//        +------+   more pending?  +-----+
//        |      +------------------+     |
//        |                               |
//        | Yes                           | No
//        |                               |
//        v                               v
//  SignalRequest()                    return
//
class Peer {
 public:
  // Initializes a peer and get its status.
  Status Init();

  // Signals that this peer has a new request to replicate/store.
  // 'force_if_queue_empty' indicates whether the peer should force
  // send the request even if the queue is empty. This is used for
  // status-only requests.
  Status SignalRequest(bool force_if_queue_empty = false);

  const metadata::QuorumPeerPB& peer_pb() const { return peer_pb_; }

  // Returns the PeerProxy if this is a remote peer or NULL if it
  // isn't. Used for tests to fiddle with the proxy and emulate remote
  // behavior.
  PeerProxy* GetPeerProxyForTests();

  void Close();

  void SetTermForTest(int term);

  ~Peer();

  // Creates a new remote peer and makes the queue track it.'
  //
  // Requests to this peer (which may end up doing IO to read non-cached
  // log entries) are assembled on 'thread_pool'.
  static Status NewRemotePeer(const metadata::QuorumPeerPB& peer_pb,
                              const std::string& tablet_id,
                              const std::string& leader_uuid,
                              PeerMessageQueue* queue,
                              ThreadPool* thread_pool,
                              gscoped_ptr<PeerProxy> proxy,
                              gscoped_ptr<Peer>* peer);

 private:
  Peer(const metadata::QuorumPeerPB& peer,
       const std::string& tablet_id,
       const std::string& leader_uuid,
       gscoped_ptr<PeerProxy> proxy,
       PeerMessageQueue* queue,
       ThreadPool* thread_pool);

  void SendNextRequest(bool even_if_queue_empty);

  // Signals that a response was received from the peer.
  void ProcessResponse();

  // Signals there was an error sending the request to the peer.
  void ProcessResponseError(const Status& status);

  std::string LogPrefixUnlocked() const;

  const std::string& tablet_id() const { return tablet_id_; }

  const std::string tablet_id_;
  const std::string leader_uuid_;

  metadata::QuorumPeerPB peer_pb_;

  gscoped_ptr<PeerProxy> proxy_;

  PeerMessageQueue* queue_;
  uint64_t failed_attempts_;

  // The last request sent.
  ConsensusRequestPB request_;
  // The last response received.
  ConsensusResponsePB response_;

  // Reference-counted pointers to any ReplicateMsgs which are in-flight to the peer. We
  // may have loaded these messages from the LogCache, in which case we are potentially
  // sharing the same object as other peers. Since the PB request_ itself can't hold
  // reference counts, this holds them.
  std::vector<ReplicateRefPtr> replicate_msg_refs_;

  rpc::RpcController controller_;

  // Held if there is an outstanding request.
  // This is used in order to ensure that we only have a single request
  // oustanding at a time, and to wait for the outstanding requests
  // at Close().
  Semaphore sem_;

  // lock that protects Peer state changes
  mutable simple_spinlock peer_lock_;

  // Heartbeater for remote peer implementations.
  // This will send status only requests to the remote peers
  // whenever we go more than 'FLAGS_leader_heartbeat_interval_ms'
  // without sending actual data.
  ResettableHeartbeater heartbeater_;

  // Thread pool used to construct requests to this peer.
  ThreadPool* thread_pool_;

  enum State {
    kPeerCreated,
    kPeerStarted,
    kPeerIdle,
    kPeerWaitingForResponse,
    kPeerClosed
  };

  State state_;
};

// A proxy to another peer. Usually a thin wrapper around an rpc proxy but can
// be replaced for tests.
class PeerProxy {
 public:

  // Sends a request, asynchronously, to a remote peer.
  virtual Status UpdateAsync(const ConsensusRequestPB* request,
                             ConsensusResponsePB* response,
                             rpc::RpcController* controller,
                             const rpc::ResponseCallback& callback) = 0;

  // Sends a RequestConsensusVote to a remote peer.
  virtual Status RequestConsensusVoteAsync(const VoteRequestPB* request,
                                           VoteResponsePB* response,
                                           rpc::RpcController* controller,
                                           const rpc::ResponseCallback& callback) = 0;

  virtual ~PeerProxy() {}
};

// A peer proxy factory. Usually just obtains peers through the rpc implementation
// but can be replaced for tests.
class PeerProxyFactory {
 public:

  virtual Status NewProxy(const metadata::QuorumPeerPB& peer_pb,
                          gscoped_ptr<PeerProxy>* proxy) = 0;

  virtual ~PeerProxyFactory() {}
};

// PeerProxy implementation that does RPC calls
class RpcPeerProxy : public PeerProxy {
 public:
  RpcPeerProxy(gscoped_ptr<HostPort> hostport,
               gscoped_ptr<ConsensusServiceProxy> consensus_proxy);

  virtual Status UpdateAsync(const ConsensusRequestPB* request,
                             ConsensusResponsePB* response,
                             rpc::RpcController* controller,
                             const rpc::ResponseCallback& callback) OVERRIDE;

  virtual Status RequestConsensusVoteAsync(const VoteRequestPB* request,
                                           VoteResponsePB* response,
                                           rpc::RpcController* controller,
                                           const rpc::ResponseCallback& callback) OVERRIDE;

  virtual ~RpcPeerProxy();
 public:
  gscoped_ptr<HostPort> hostport_;
  gscoped_ptr<ConsensusServiceProxy> consensus_proxy_;
};

// PeerProxyFactory implementation that generates RPCPeerProxies
class RpcPeerProxyFactory : public PeerProxyFactory {
 public:
  explicit RpcPeerProxyFactory(const std::tr1::shared_ptr<rpc::Messenger>& messenger);

  virtual Status NewProxy(const metadata::QuorumPeerPB& peer_pb,
                          gscoped_ptr<PeerProxy>* proxy) OVERRIDE;

  virtual ~RpcPeerProxyFactory();
 private:
  std::tr1::shared_ptr<rpc::Messenger> messenger_;
};

// Query the consensus service at last known host/port that is
// specified in 'remote_peer' and set the 'permanent_uuid' field based
// on the response.
Status SetPermanentUuidForRemotePeer(const std::tr1::shared_ptr<rpc::Messenger>& messenger,
                                     metadata::QuorumPeerPB* remote_peer);

}  // namespace consensus
}  // namespace kudu

#endif /* KUDU_CONSENSUS_CONSENSUS_PEERS_H_ */
