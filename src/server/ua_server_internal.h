#ifndef UA_SERVER_INTERNAL_H_
#define UA_SERVER_INTERNAL_H_

#include "ua_util.h"
#include "ua_server.h"
#include "ua_server_external_ns.h"
#include "ua_session_manager.h"
#include "ua_securechannel_manager.h"
#include "ua_nodestore.h"

#ifdef UA_ENABLE_SUBSCRIPTIONS
#include "ua_subscription_manager.h"
#endif

#define PRODUCT_URI "http://open62541.org"
#define ANONYMOUS_POLICY "open62541-anonymous-policy"
#define USERNAME_POLICY "open62541-username-policy"

#ifdef UA_ENABLE_EXTERNAL_NAMESPACES
/** Mapping of namespace-id and url to an external nodestore. For namespaces
    that have no mapping defined, the internal nodestore is used by default. */
typedef struct UA_ExternalNamespace {
	UA_UInt16 index;
	UA_String url;
	UA_ExternalNodeStore externalNodeStore;
} UA_ExternalNamespace;
#endif

struct UA_Server {
    /* Config */
    UA_ServerConfig config;
    UA_Logger logger;

    /* Meta */
    UA_DateTime startTime;
    UA_DateTime buildDate;
    UA_ApplicationDescription description;
    size_t endpointDescriptionsSize;
    UA_EndpointDescription *endpointDescriptions;

    /* Communication */
    size_t networkLayersSize;
    UA_ServerNetworkLayer **networkLayers;

    /* Security */
    UA_ByteString serverCertificate;
    UA_SecureChannelManager secureChannelManager;
    UA_SessionManager sessionManager;

    /* Address Space */
    UA_NodeStore *nodestore;
    size_t namespacesSize;
    UA_String *namespaces;

#ifdef UA_ENABLE_EXTERNAL_NAMESPACES
    size_t externalNamespacesSize;
    UA_ExternalNamespace *externalNamespaces;
#endif
     
    /* Jobs with a repetition interval */
    LIST_HEAD(RepeatedJobsList, RepeatedJobs) repeatedJobs;
    
#ifdef UA_ENABLE_MULTITHREADING
    /* Dispatch queue head for the worker threads (the tail should not be in the same cache line) */
	struct cds_wfcq_head dispatchQueue_head;

    UA_Boolean *running;
    UA_UInt16 nThreads;
    UA_UInt32 **workerCounters;
    pthread_t *thr;

    struct cds_lfs_stack mainLoopJobs; /* Work that shall be executed only in the main loop and not
                                          by worker threads */
    struct DelayedJobs *delayedJobs;

    pthread_cond_t dispatchQueue_condition; /* so the workers don't spin if the queue is empty */
    /* Dispatch queue tail for the worker threads */
	struct cds_wfcq_tail dispatchQueue_tail;
#endif
};

/* The node is assumed to be "finished", i.e. no instantiation from inheritance is necessary */
void UA_Server_addExistingNode(UA_Server *server, UA_Session *session, UA_Node *node,
                               const UA_NodeId *parentNodeId, const UA_NodeId *referenceTypeId,
                               UA_AddNodesResult *result);

typedef UA_StatusCode (*UA_EditNodeCallback)(UA_Server*, UA_Session*, UA_Node*, const void*);

/* Calls callback on the node. In the multithreaded case, the node is copied before and replaced in
   the nodestore. */
UA_StatusCode UA_Server_editNode(UA_Server *server, UA_Session *session, const UA_NodeId *nodeId,
                                 UA_EditNodeCallback callback, const void *data);

void UA_Server_processBinaryMessage(UA_Server *server, UA_Connection *connection, const UA_ByteString *msg);

UA_StatusCode UA_Server_addDelayedJob(UA_Server *server, UA_Job job);

void UA_Server_deleteAllRepeatedJobs(UA_Server *server);

/***********/
/* Hashing */
/***********/

typedef UA_UInt32 hash_t;
hash_t hash_array(const UA_Byte *data, size_t len, hash_t seed);
hash_t hash_type(const void *p, const UA_DataType *type, hash_t seed);
hash_t hash_nodeid(const UA_NodeId *n);

#endif /* UA_SERVER_INTERNAL_H_ */
