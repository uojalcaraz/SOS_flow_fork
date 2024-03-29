
#include "sos.h"
#include "sos_debug.h"
#include "sos_error.h"
#include "sosd.h"
#include "sosd_cloud_socket.h"
#include "string.h"
#include "sos_target.h"

bool SOSD_cloud_shutdown_underway = false;

bool SOSD_sockets_ready_to_listen = false;
void SOSD_socket_register_connection(SOS_buffer *msg);

void SOSD_cloud_listen_loop(void) {
    SOS_SET_CONTEXT(SOSD.sos_context, "SOSD_cloud_listen_loop");
    // NOTE: This is a thread launched from sosd.c
    while (SOSD_sockets_ready_to_listen == false ) {
        usleep (500000);
    }

    SOS_buffer *msg = NULL;
    SOS_buffer_init(SOSD.sos_context, &msg);

    SOS_buffer *ack = NULL;
    SOS_buffer_init_sized_locking(SOSD.sos_context, &ack, 2048, false);
    SOSD_PACK_ACK(ack);

    while (SOS->status == SOS_STATUS_RUNNING) {
		SOS_buffer_wipe(msg);
		//
		dlog(1, "Listening for messages from other daemons...\n");
        SOS_target_accept_connection(SOSD.daemon.cloud_inlet);
        dlog(1, "Receiving message...\n");
        SOS_target_recv_msg(SOSD.daemon.cloud_inlet, msg);
        dlog(1, "Sending ACK message...\n");
        SOS_target_send_msg(SOSD.daemon.cloud_inlet, ack);
        dlog(1, "Disconnecting...\n");
        SOS_target_disconnect(SOSD.daemon.cloud_inlet);
        //
        dlog(1, "Message received!  Processing...\n");
        SOSD_cloud_process_buffer(msg);
    }
    
    return;
}


//Process a buffer containing 1 messages...
void SOSD_cloud_process_buffer(SOS_buffer *buffer) {
    SOS_SET_CONTEXT(SOSD.sos_context, "SOSD_cloud_process_buffer");

    dlog(1, "SOSD_cloud_process_buffer\n");
    SOS_msg_header    header;

    int displaced    = 0;
    int offset       = 0;

    //NOTE: Sockets only ever have a single message, like other
    //      SOS point to point communications.
    
    memset(&header, '\0', sizeof(SOS_msg_header));

    SOS_msg_unzip(buffer, &header, offset, &offset);

    dlog(1, "     ... header.msg_size == %d\n",
        header.msg_size);
    dlog(1, "     ... header.msg_type == %s  (%d)\n",
        SOS_ENUM_STR(header.msg_type, SOS_MSG_TYPE), header.msg_type);
    dlog(1, "     ... header.msg_from == %" SOS_GUID_FMT "\n",
        header.msg_from);
    dlog(1, "     ... header.ref_guid == %" SOS_GUID_FMT "\n",
        header.ref_guid);

    //Create a new message buffer:
    SOS_buffer *msg;
    SOS_buffer_init_sized_locking(SOS, &msg, (1 + header.msg_size), false);

    //Copy the data into the new message directly:
    memcpy(msg->data, buffer->data, header.msg_size);
    msg->len = header.msg_size;
    offset += header.msg_size;

    //Enqueue this new message into the local_sync:
    switch (header.msg_type) {
        case SOS_MSG_TYPE_ANNOUNCE:
        case SOS_MSG_TYPE_PUBLISH:
        case SOS_MSG_TYPE_VAL_SNAPS:
            pthread_mutex_lock(SOSD.sync.local.queue->sync_lock);
            pipe_push(SOSD.sync.local.queue->intake, &msg, 1);
            SOSD.sync.local.queue->elem_count++;
            pthread_mutex_unlock(SOSD.sync.local.queue->sync_lock);
            break;

        case SOS_MSG_TYPE_REGISTER:
            SOSD_socket_register_connection(msg);
            break;

        case SOS_MSG_TYPE_SHUTDOWN:
            SOSD.daemon.running = 0;
            SOSD.sos_context->status = SOS_STATUS_SHUTDOWN;
            SOS_buffer *shutdown_msg;
            SOS_buffer *shutdown_rep;
            SOS_buffer_init_sized_locking(SOS, &shutdown_msg, 1024, false);
            SOS_buffer_init_sized_locking(SOS, &shutdown_rep, 1024, false);
            int offset_shut = 0;
            SOS_buffer_pack(shutdown_msg, &offset_shut, "i", offset_shut);
            SOSD_send_to_self(shutdown_msg, shutdown_rep);
            SOS_buffer_destroy(shutdown_msg);
            SOS_buffer_destroy(shutdown_rep);
            break;

        case SOS_MSG_TYPE_TRIGGERPULL:
            SOSD_cloud_handle_triggerpull(msg);
            break;

        case SOS_MSG_TYPE_ACK:
            dlog(1, "sosd(%d) received ACK message"
                " from rank %" SOS_GUID_FMT " !\n",
                    SOSD.sos_context->config.comm_rank, header.msg_from);
            break;

        default:    SOSD_handle_unknown    (msg); break;
    }

    return;
}


void SOSD_socket_register_connection(SOS_buffer *msg) {
    SOS_SET_CONTEXT(SOSD.sos_context, "SOSD_socket_register_connection");

    dlog(1, "Registering a new connection...\n");

    SOS_msg_header header;
    int offset = 0;
    SOS_buffer_unpack(msg, &offset, "iigg",
        &header.msg_size,
        &header.msg_type,
        &header.msg_from,
        &header.ref_guid);

    SOS_socket *inlet = SOSD.daemon.cloud_inlet;

    if (header.msg_from >= SOS->config.comm_size) {
        fprintf(stderr, "ERROR: You are attempting to register a rank"
                "(%" SOS_GUID_FMT ") outside the size (%d) that you"
                " specified to the daemon at launch.",
                header.msg_from,
                SOS->config.comm_size);
        fflush(stderr);
    }

    char *remote_host = NULL;
    int   remote_port;

    SOS_buffer_unpack_safestr(msg, &offset, &remote_host);
    SOS_buffer_unpack(msg, &offset, "i", &remote_port);

    SOS_socket *rmt_tgt = NULL;

    SOS_target_init(SOS, &rmt_tgt, remote_host, remote_port);
    SOSD.daemon.cloud_targets[header.msg_from] = rmt_tgt;

    dlog(1, "   ... sosd(%" SOS_GUID_FMT ") is monitoring cloud on %s:%d\n",
        header.msg_from,
        remote_host,
        remote_port);
    
    free(remote_host);

    dlog(1, "done.\n");

    // Send them back an ACK (this is OOB from normal message
    // handshaking, and is used to coordinate init like a barrier).

    SOS_buffer *reply;
    SOS_buffer_init_sized_locking(SOS, &reply, 128, false);

    header.msg_size = -1;
    header.msg_type = SOS_MSG_TYPE_ACK;
    header.msg_from = SOS->config.comm_rank;
    header.ref_guid = 0;

    offset = 0;
    SOS_buffer_pack(reply, &offset, "iigg",
        header.msg_size,
        header.msg_type,
        header.msg_from,
        header.ref_guid);

    header.msg_size = offset;
    offset = 0;
    SOS_buffer_pack(reply, &offset, "i",
        header.msg_size);

    SOS_target_connect(rmt_tgt);
    SOS_target_send_msg(rmt_tgt, reply);
    SOS_target_recv_msg(rmt_tgt, reply);
    SOS_target_disconnect(rmt_tgt);

    dlog(3, "Registration complete.\n");

    return;
}


// NOTE: Trigger pulls do not flow out beyond the node where
//       they are pulled (at this time).  They go "downstream"
//       from AGGREGATOR->LISTENER and LISTENER->LOCALAPPS
void SOSD_cloud_handle_triggerpull(SOS_buffer *msg) {
    SOS_SET_CONTEXT(msg->sos_context, "SOSD_cloud_handle_triggerpull");

    dlog(4, "Message received... unzipping.\n");

    SOS_msg_header header;
    int offset = 0;
    SOS_msg_unzip(msg, &header, 0, &offset);

    int offset_after_original_header = offset;

    dlog(4, "Done unzipping.  offset_after_original_header == %d\n", 
            offset_after_original_header);

    
    if ((SOS->role == SOS_ROLE_AGGREGATOR)
     && (SOS->config.comm_size > 1)) {
        
        dlog(4, "I am an aggregator, and I have some"
                " listener[s] to notify.\n");

        buffer_rec rec;

        dlog(2, "Wrapping the trigger message...\n");

        SOS_buffer *wrapped_msg;
        SOS_buffer_init_sized_locking(SOS, &wrapped_msg, (msg->len + 4 + 1), false);
        
        header.msg_size = msg->len;
        header.msg_type = SOS_MSG_TYPE_TRIGGERPULL;
        header.msg_from = SOS->config.comm_rank;
        header.ref_guid = 0;

        offset = 0;

        SOS_buffer_grow(wrapped_msg, msg->len, SOS_WHOAMI);
        memcpy(wrapped_msg->data,
                msg->data,
                msg->len);
        wrapped_msg->len = (msg->len);
        offset = wrapped_msg->len;

        header.msg_size = offset;
        offset = 0;
        dlog(4, "Tacking on the newly wrapped message size...\n");
        dlog(4, "   header.msg_size == %d\n", header.msg_size);
        SOS_buffer_pack(wrapped_msg, &offset, "i",
            header.msg_size);

        dlog(1, "SOSD_cloud_handle_triggerpull loop \n");
        //Send trigger only to the listeners of this aggregator
        int id = 0;
        for (id = 0; id < SOS->config.comm_size; id++) {
            dlog(1, "SOSD_cloud_handle_triggerpull id %d\n", id);
            if(SOSD.daemon.cloud_targets[id]==NULL)
            {
                dlog(1, " skipping id %d, not registered in this aggregator\n", id);
                continue;
            }

            SOS_socket *rmt_tgt = NULL;
            rmt_tgt = SOSD.daemon.cloud_targets[id];
            
            // NOTE: See SOSD_cloud_enqueue() for async sends.
            dlog(1, "SOSD_cloud_handle_triggerpull send to %s:%s\n", rmt_tgt->remote_host, rmt_tgt->remote_port);
            SOS_target_connect(rmt_tgt);
            int bytes_sent = SOS_target_send_msg(rmt_tgt, wrapped_msg);
            dlog(1, "Number of bytes sent: %d\n", bytes_sent);
            SOS_target_disconnect(rmt_tgt);

        }
    }

    // Both Aggregators and Listeners should drop the feedback into
    // their queues in case they have local processes that have
    // registered sensitivity...
   
    offset = offset_after_original_header;

    char *handle = NULL;
    char *message = NULL;
    int message_len = -1;

    SOS_buffer_unpack_safestr(msg, &offset, &handle);
    SOS_buffer_unpack(msg, &offset, "i", &message_len);
    SOS_buffer_unpack_safestr(msg, &offset, &message);

    //fprintf(stderr, "sosd(%d) got a TRIGGERPULL message from"
    //        " sosd(%" SOS_GUID_FMT ") of %d bytes in length.\n",
    //        SOS->config.comm_rank,
    //        header.msg_from,
    //        header.msg_size);
    //fflush(stderr);

    SOSD_feedback_task *task;
    task = calloc(1, sizeof(SOSD_feedback_task));
    task->type = SOS_FEEDBACK_TYPE_PAYLOAD;
    SOSD_feedback_payload *payload = calloc(1, sizeof(SOSD_feedback_payload));

    payload->handle = handle;
    payload->size = message_len;
    payload->data = (void *) message;

    //fprintf(stderr, "sosd(%d) enquing the following task->ref:\n"
    //        "   payload->handle == %s\n"
    //        "   payload->size   == %d\n"
    //        "   payload->data   == \"%s\"\n",
    //        SOSD.sos_context->config.comm_rank,
    //        payload->handle,
    //        payload->size,
    //        (char*) payload->data);
    //fflush(stderr);

    task->ref = (void *) payload;    
    pthread_mutex_lock(SOSD.sync.feedback.queue->sync_lock);
    pipe_push(SOSD.sync.feedback.queue->intake, (void *) &task, 1);
    SOSD.sync.feedback.queue->elem_count++;
    pthread_mutex_unlock(SOSD.sync.feedback.queue->sync_lock);

    
    return;
}


/* name.........: SOSD_cloud_init
 * parameters...: argc, argv (passed in by address)
 * return val...: 0 if no errors
 * description..:
 *     This routine stands up the off-node transport services for the daemon
 *     and launches any particular threads it needs to in order to do that.
 *
 *     In the MPI-version, this function is responsible for populating the
 *     following global values.  Some reasonable values will at least need
 *     to be plugged into the SOS->config.* variables.
 *
 *        SOS->config.comm_rank
 *        SOS->config.comm_size
 *        SOS->config.comm_support = MPI_THREAD_*
 *        SOSD.daemon.cloud_sync_target_set[n]  (int: rank)
 *        SOSD.daemon.cloud_sync_target_count
 *        SOSD.daemon.cloud_sync_target
 *
 *    The SOSD.daemon.cloud_sync stuff can likely change here, if EVPATH
 *    is going to handle it's business differently.  The sync_target refers
 *    to the centralized store (here, stone?) that this daemon is pointing to
 *    for off-node transport.  The general system allows for multiple "back-
 *    plane stores" launched alongside the daemons, to provide reasonable
 *    scalability and throughput.
 */
int SOSD_cloud_init(int *argc, char ***argv) {
    SOS_SET_CONTEXT(SOSD.sos_context, "SOSD_cloud_init.SOCKET");
    dlog(1, "Initializing SOCKET based inter-daemon cloud...\n");
    SOSD_sockets_ready_to_listen = false;
    
    SOS_options *opt = SOSD.sos_context->config.options;
    if (opt == NULL) {
        printf("opt is null");
    }
    if ((opt->discovery_dir == NULL)) { // || (strlen(opt->discovery_dir) < 1)) {
        opt->discovery_dir = (char *) calloc(sizeof(char), PATH_MAX);
        if (!getcwd(opt->discovery_dir, PATH_MAX)) {
            fprintf(stderr, "ERROR: The SOS_DISCOVERY_DIR evar was not set,"
                    " and getcwd() failed! Set the evar and retry.\n");
            fflush(stderr);
            exit(EXIT_FAILURE);
        }
        fprintf(stderr, "STATUS: The SOS_DISCOVERY_DIR evar was not set."
                " Using getcwd() path:\n\t%s\n", opt->discovery_dir);
        fflush(stderr);
    }

    int expected_node_count =
        SOSD.daemon.aggregator_count + 
        SOSD.daemon.listener_count;

    SOS->config.comm_size = expected_node_count;
    SOS->config.comm_support = -1; // Used for MPI only.

    // Do some sanity checks.
    if (SOSD.daemon.aggregator_count == 0) {
        fprintf(stderr, "ERROR: SOS requires an aggregator.\n");
        fflush(stderr);
        exit(EXIT_FAILURE);
    }
    if ((SOS->config.comm_rank < 0)
        || (SOS->config.comm_rank >= expected_node_count)) {
        fprintf(stderr, "ERROR: SOS rank %d is outside the bounds of"
                " ranks expected (%d).\n",
                SOS->config.comm_rank,
                expected_node_count);
        fflush(stderr);
        exit(EXIT_FAILURE);
    }
    if ((SOS->role == SOS_ROLE_LISTENER)
        && (SOS->config.comm_rank < SOSD.daemon.aggregator_count)) {
        fprintf(stderr, "ERROR: SOS listener(%d) was assigned a rank"
                " inside the range reserved for aggregators (0-%d).\n",
                SOS->config.comm_rank,
                (SOSD.daemon.aggregator_count - 1));
        fflush(stdout);
        exit(EXIT_FAILURE);
    }
    if ((SOS->role == SOS_ROLE_AGGREGATOR)
        && (SOS->config.comm_rank >= SOSD.daemon.aggregator_count)) {
        fprintf(stderr, "ERROR: SOS aggregator(%d) was assigned a rank"
                " outside the range reserved for aggregators (0-%d).\n",
                SOS->config.comm_rank,
                (SOSD.daemon.aggregator_count - 1));
        fflush(stderr);
        exit(EXIT_FAILURE);
    }

    SOSD.daemon.cloud_sync_target_count = SOSD.daemon.aggregator_count;

    dlog(1, "Initializing SOCKET --after sanity checks-- based inter-daemon cloud...\n");

    int aggregation_rank = -1;
    if (SOSD.sos_context->role == SOS_ROLE_AGGREGATOR) {
        aggregation_rank = SOSD.sos_context->config.comm_rank;
        SOSD.daemon.cloud_sync_target = -1;
    } else {
        aggregation_rank = SOSD.sos_context->config.comm_rank
            % SOSD.daemon.aggregator_count;
        SOSD.daemon.cloud_sync_target = aggregation_rank;
    }
    dlog(1, "   ... aggregation_rank: %d\n", aggregation_rank);

    char *contact_filename = (char *) calloc(2048, sizeof(char));
    snprintf(contact_filename, 2048, "%s/sosd.%05d.key",
        opt->discovery_dir, aggregation_rank);
    dlog(1, "   ... contact_filename: %s\n", contact_filename);

    char *present_filename = (char *) calloc(2048, sizeof(char));
    snprintf(present_filename, 2048, "%s/sosd.%05d.id",
        opt->discovery_dir, SOSD.sos_context->config.comm_rank);
    dlog(1, "   ... present_filename: %s\n", present_filename);


    SOS_socket *tgt = NULL;
    SOS_target_init(SOS, &tgt,
            SOS->config.daemon_host,
            0);
    SOS_target_setup_for_accept(tgt);

    //Gather some information about this socket:
    struct sockaddr_in sin;
    socklen_t sin_len = sizeof(sin);
    getsockname(tgt->local_socket_fd, (struct sockaddr *)&sin, &sin_len);
    int sosd_msg_port = ntohs(sin.sin_port);
    
    dlog(0, "SOSD[socket] -- listening on port: %d\n", sosd_msg_port);
    
    SOSD.daemon.cloud_inlet = tgt;
    snprintf(SOSD.daemon.cloud_inlet->local_port, NI_MAXSERV, "%d", sosd_msg_port);
    SOSD.daemon.cloud_inlet->port_number = sosd_msg_port;
    dlog(1, "SOSD.daemon.cloud_inlet->local_port : %s\n", SOSD.daemon.cloud_inlet->local_port);
    dlog(1, "SOSD.daemon.cloud_inlet->port_number : %d\n", SOSD.daemon.cloud_inlet->port_number);    

    if (SOSD.sos_context->role == SOS_ROLE_AGGREGATOR) {

        // AGGREGATOR
        //   ... the aggregator needs to wait on the registration messages
        //   before being able to create sending stones.

        dlog(0, "   ... demon role: AGGREGATOR\n");
        // Make space to track connections back to the listeners:
        dlog(0, "   ... creating objects to coordinate with listeners: ");
        SOSD.daemon.cloud_targets = (SOS_socket **)
            calloc(expected_node_count, sizeof(SOS_socket *));
        int id;
        for(id = 0; id < expected_node_count; id++)
            SOSD.daemon.cloud_targets[id] = NULL;

        dlog(0, "done.\n");

        FILE *contact_file;
		// set the node id before we use it.
		SOSD.sos_context->config.node_id = (char *)
            calloc(NI_MAXHOST, sizeof(char));
		gethostname(SOSD.sos_context->config.node_id, NI_MAXHOST);
        contact_file = fopen(contact_filename, "w");

        dlog(0, "fprintf.\n");
        fprintf(contact_file, "%s\n%s\n",
                SOSD.daemon.cloud_inlet->local_host,
                SOSD.daemon.cloud_inlet->local_port);
        dlog(0, "fflush.\n");
        fflush(contact_file);
        dlog(0, "fclose.\n");
        fclose(contact_file);
        dlog(0, "done.\n");

    } else {

        //LISTENER

        dlog(0, "   ... waiting for coordinator to share contact"
                " information.\n");
        while (!SOS_file_exists(contact_filename)) {
            usleep(100000);
        }

        char remote_host[NI_MAXHOST] = {0};
        char remote_port[NI_MAXSERV] = {0};

        while(strnlen(remote_host, NI_MAXHOST) < 1) {
            FILE *contact_file;
            contact_file = fopen(contact_filename, "r");
            if (contact_file < 0) {
                dlog(1, "   ... could not open contact file %s yet. (%d)\n",
                        contact_filename, contact_file);
                usleep(500000);
                continue;
            }
            int rc = 0;
            rc = fscanf(contact_file, "%s\n%s",
                    remote_host, remote_port);
            if (strnlen(remote_host, NI_MAXHOST) < 1) {
                dlog(1, "   ... could not read contact key file yet.\n");
            }
            fclose(contact_file);
            usleep(500000);
        }

        dlog(0, "   ... targeting aggregator at: %s:%s\n",
                remote_host, remote_port);
        dlog(0, "done.\n");
        if (SOSD.daemon.cloud_aggregator == NULL) {
            SOS_target_init(SOS, &(SOSD.daemon.cloud_aggregator), remote_host, atoi(remote_port));
        }


        SOS_buffer *buffer;
        SOS_buffer_init_sized_locking(SOS, &buffer, 2048, false);

        SOS_msg_header header;
        header.msg_size = -1;
        header.msg_type = SOS_MSG_TYPE_REGISTER;
        header.msg_from = SOSD.sos_context->config.comm_rank;
        header.ref_guid = 0;



        dlog(1, " header.msg_from : %d\n",  header.msg_from);
        dlog(1, "header.ref_guid : %d\n", header.ref_guid);

        int offset = 0;

        SOS_buffer_pack(buffer, &offset, "iigg",
            header.msg_size,
            header.msg_type,
            header.msg_from,
            header.ref_guid);

        SOS_buffer_pack(buffer, &offset, "si", tgt->local_host, tgt->port_number);
        
        header.msg_size = offset;
        offset = 0;
      
        SOS_buffer_pack(buffer, &offset, "i",
            header.msg_size);


        SOSD_cloud_send(buffer, NULL);
        SOS_buffer_destroy(buffer);
    }

    FILE *present_file;
    // set the node id before we use it.
    present_file = fopen(present_filename, "w");
    fprintf(present_file, "%s\n%s\n%d\n",
            SOSD.daemon.cloud_inlet->local_host,
            SOSD.daemon.cloud_inlet->local_port,
            sosd_msg_port);
    fflush(present_file);
    fclose(present_file);

    SOSD_sockets_ready_to_listen = true;
    free(contact_filename);
    free(present_filename);
    dlog(0, "   ... done.\n");

    return 0;
}


/* name.......: SOSD_cloud_start
 * description: In the event that initialization and activation are not
 *    necessarily the same, when this function returns the communication
 *    between sosd instances is active, and all cloud functions are
 *    operating.
 */
int SOSD_cloud_start(void) {
    
    //TODO
    return 0;
}



/* name.......: SOSD_cloud_send
 * description: Send a message to the target aggregator.
 */
int SOSD_cloud_send(SOS_buffer *buffer, SOS_buffer *reply) {
    SOS_SET_CONTEXT(SOSD.sos_context, "SOSD_cloud_send.SOCKET");

    // NOTE: See SOSD_cloud_enqueue() for async sends.
    dlog(1, "SOSD_cloud_send\n");
    dlog(1, "-----------> ----> -------------> ----------> ------------->\n");
    dlog(1, "----> --> >>Transporting off-node!>> ---------------------->\n");
    dlog(1, "---------------> ---------> --------------> ----> -----> -->\n");
    SOS_buffer *reply_ptr;

    if (reply == NULL) {
        reply_ptr = NULL;
        SOS_buffer_init_sized_locking(SOS, &reply_ptr, 128, false);
    } else {
        reply_ptr = reply;
    }
    SOS_target_connect(SOSD.daemon.cloud_aggregator);
    dlog(1, "Sending message to target aggregator\n");
    int bytes_sent = SOS_target_send_msg(SOSD.daemon.cloud_aggregator, buffer);
    dlog(1, "Sent %d bytes to target aggregator\n", bytes_sent);
    SOS_target_recv_msg(SOSD.daemon.cloud_aggregator, reply_ptr);
    dlog(1, "received ACK from target aggregator\n", bytes_sent);
    SOS_target_disconnect(SOSD.daemon.cloud_aggregator);

    if (reply == NULL) {
        SOS_buffer_destroy(reply_ptr);
    }

    return 0;
}


/* name.......: SOSD_cloud_enqueue
 * description: Accept a message into the async send-queue.  (non-blocking)
 *              The purpose of this abstraction is to eventually allow
 *              SOSD to manage the bundling of multiple messages before
 *              passing them off to the underlying transport API.
 */
void  SOSD_cloud_enqueue(SOS_buffer *buffer) {
    SOS_SET_CONTEXT(SOSD.sos_context, "SOSD_cloud_enqueue.SOCKET");
    dlog(1, "SOSD_cloud_enqueue\n");
    dlog(1, "-----------> ----> -------------> ----------> ------------->\n");
    dlog(1, "----> --> >>Transporting off-node!>> ---------------------->\n");
    dlog(1, "---------------> ---------> --------------> ----> -----> -->\n");


    SOS_msg_header header;
    int offset;

    if (SOSD_cloud_shutdown_underway) { return; }
    if (buffer->len == 0) {
        dlog(1, "ERROR: You attempted to enqueue a zero-length message.\n");
        return;
    }

    memset(&header, '\0', sizeof(SOS_msg_header));

    offset = 0;
    SOS_buffer_unpack(buffer, &offset, "iigg",
                      &header.msg_size,
                      &header.msg_type,
                      &header.msg_from,
                      &header.ref_guid);

    dlog(6, "Enqueueing a %s message of %d bytes...\n",
            SOS_ENUM_STR(header.msg_type, SOS_MSG_TYPE), header.msg_size);
    if (buffer->len != header.msg_size) { dlog(1, "  ... ERROR:"
            " buffer->len(%d) != header.msg_size(%d)",
            buffer->len, header.msg_size); }

    pthread_mutex_lock(SOSD.sync.cloud_send.queue->sync_lock);
    pipe_push(SOSD.sync.cloud_send.queue->intake, (void *) &buffer, sizeof(SOS_buffer *));
    SOSD.sync.cloud_send.queue->elem_count++;
    pthread_mutex_unlock(SOSD.sync.cloud_send.queue->sync_lock);

    dlog(1, "  ... done.\n");
   return;
}


// name.......: SOSD_cloud_fflush
// description: Force the send-queue to flush and transmit.
//
void  SOSD_cloud_fflush(void) {
    SOS_SET_CONTEXT(SOSD.sos_context, "SOSD_cloud_fflush.SOCKET");

    //TODO: Decide whether this op is supported.

    return;
}


/* name.......: SOSD_cloud_finalize
 * description: Shut down the cloud operation, flush / close files, etc.
 */
int   SOSD_cloud_finalize(void) {
    SOS_SET_CONTEXT(SOSD.sos_context, "SOSD_cloud_finalize.SOCKET");

    if (SOSD.sos_context->role != SOS_ROLE_AGGREGATOR) {
        return 0;
    }
    char *contact_filename = (char *) calloc(2048, sizeof(char));
    snprintf(contact_filename, 2048, "%s/sosd.%05d.key",
        SOS->config.options->discovery_dir, SOS->config.comm_rank);
    dlog(1, "   Removing key file: %s\n", contact_filename);

    if (remove(contact_filename) == -1) {
        dlog(0, "   Error, unable to delete key file!\n");
    }

    return 0;
}


/* name.......: SOSD_cloud_shutdown_notice
 * description: Send notifications to any daemon ranks that are not in the
 *              business of listening to the node on the SOS_CMD_PORT socket.
 *              Only certain daemon ranks participate/call this function.
 */
void  SOSD_cloud_shutdown_notice(void) {
    SOS_SET_CONTEXT(SOSD.sos_context, "SOSD_cloud_shutdown_notice.SOCKET");

    SOS_buffer *shutdown_msg;
    SOS_buffer_init(SOS, &shutdown_msg);

    dlog(1, "Providing shutdown notice to the cloud_sync backend...\n");
    SOSD_cloud_shutdown_underway = true;

    if ((SOS->config.comm_rank - SOSD.daemon.cloud_sync_target_count)
            < SOSD.daemon.cloud_sync_target_count)
    {
        dlog(1, "  ... preparing notice to SOS_ROLE_AGGREGATOR at"
                " rank %d\n", SOSD.daemon.cloud_sync_target);
        // LISTENER:
        // The first N listener ranks will notify the N aggregators...
        //
        SOS_msg_header header;
        SOS_buffer    *shutdown_msg;
        SOS_buffer    *reply;
        int            embedded_msg_count;
        int            msg_inset;

        SOS_buffer_init(SOS, &shutdown_msg);
        SOS_buffer_init_sized_locking(SOS, &reply, 10, false);

        header.msg_size = -1;
        header.msg_type = SOS_MSG_TYPE_SHUTDOWN;
        header.msg_from = SOS->my_guid;
        header.ref_guid = 0;

        int offset = 0;
        
        header.msg_size = SOS_buffer_pack(shutdown_msg, &offset, "iigg",
                                          header.msg_size,
                                          header.msg_type,
                                          header.msg_from,
                                          header.ref_guid);
        offset = 0;
        SOS_buffer_pack(shutdown_msg, &offset, "i", header.msg_size);

        dlog(1, "  ... sending notice\n");
        SOSD_cloud_send(shutdown_msg, reply); 
        dlog(1, "  ... sent successfully\n");

        SOS_buffer_destroy(shutdown_msg);
        SOS_buffer_destroy(reply);

    } else {
        // AGGREGATOR:
        //     Build a dummy message to send to ourself to purge the
        //     main listen loop's accept() call:
        dlog(1, "Processing a self-send to flush main listen"
                " loop's socket accept()...\n");

        SOS_msg_header header;
        int offset;

        SOS_buffer *shutdown_msg = NULL;
        SOS_buffer *shutdown_reply = NULL;
        SOS_buffer_init(SOS, &shutdown_msg);
        SOS_buffer_init(SOS, &shutdown_reply);

        header.msg_size = -1;
        header.msg_type = SOS_MSG_TYPE_SHUTDOWN;
        header.msg_from = SOS->my_guid;
        header.ref_guid = -1;

        offset = 0;
        SOS_msg_zip(shutdown_msg, header, 0, &offset);

        header.msg_size = offset;
        offset = 0;
        SOS_msg_zip(shutdown_msg, header, 0, &offset);

        SOSD_send_to_self(shutdown_msg, shutdown_reply);
        SOS_buffer_destroy(shutdown_msg);
        SOS_buffer_destroy(shutdown_reply);
    }
    
    dlog(1, "  ... done\n");

    return;
}


