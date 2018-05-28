/******************************************************************************
 * ctcp.c
 * ------
 * Implementation of cTCP done here. This is the only file you need to change.
 * Look at the following files for references and useful functions:
 *   - ctcp.h: Headers for this file.
 *   - ctcp_iinked_list.h: Linked list functions for managing a linked list.
 *   - ctcp_sys.h: Connection-related structs and functions, cTCP segment
 *                 definition.
 *   - ctcp_utils.h: Checksum computation, getting the current time.
 *
 *****************************************************************************/

#include "ctcp.h"
#include "ctcp_linked_list.h"
#include "ctcp_sys.h"
#include "ctcp_utils.h"

/******************************************************************************
 * Variable/struct declarations
 *****************************************************************************/

/**
 * ctcp_state child structs
 */
typedef struct {
  uint32_t last_ackno_rxed;
  bool has_EOF_been_read;

  /* This should be the index of the LAST byte we have read from conn_input(). */
  uint32_t last_seqno_read;

  /* If the newest segment we've sent (not including rexmits) has a sequence
  ** number of 1000 and a length of 10 bytes, then last_seqno_sent should be
  ** 1010. I.e., it's the sequence number of the last byte we've sent.  */
  uint32_t last_seqno_sent;

  /* If this is too old, we can send a keepalive. */
  long timestamp_of_last_send;

  /* Make this point to a metadata wrapper:
  **     --> wrapped_segment:
  **             - num xmits
  **             - timestamp of last send
  **             - ctcp_segment struct (see ctcp_sys.h)  */
  linked_list_t* wrapped_unacked_segments;
} tx_state_t;

typedef struct {
  uint32_t last_seqno_accepted; /* Use this to generate ackno's when sending. */
  bool has_FIN_been_rxed;
  uint32_t num_truncated_segments;
  uint32_t num_out_of_order_segments;
  uint32_t num_invalid_cksums;

  /* This should be a linked list of ctcp_segment_t*'s.  */
  linked_list_t* segments_to_output;
} rx_state_t;

typedef struct {
  uint32_t         num_xmits;
  long             timestamp_of_last_send;
  ctcp_segment_t   ctcp_segment;
} wrapped_ctcp_segment_t;

/**
 * Connection state.
 *
 * Stores per-connection information such as the current sequence number,
 * unacknowledged packets, etc.
 *
 * You should add to this to store other fields you might need.
 */
struct ctcp_state {
  struct ctcp_state *next;  /* Next in linked list */
  struct ctcp_state **prev; /* Prev in linked list */
  conn_t *conn;             /* Connection object -- needed in order to figure
                               out destination when sending */
  ctcp_config_t ctcp_config;
  tx_state_t tx_state;
  rx_state_t rx_state;
};

/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;

/* FIXME: Feel free to add as many helper functions as needed. Don't repeat
          code! Helper functions make the code clearer and cleaner. */

/******************************************************************************
 * Local function declarations.
 *****************************************************************************/

/**
 * This is to be called by ctcp_read() and ctcp_timer(). This function is
 * responsible for examining 'state', and xmiting (or rexmiting) as many
 * segments as possible.
 */
void ctcp_send_what_we_can(ctcp_state_t *state);

/**
 * Sends 'wrapped_segment' and updates 'state' accordingly.
 */
void ctcp_send_segment(ctcp_state_t *state, wrapped_ctcp_segment_t* wrapped_segment);


/**
 * This should be called after tx_state.last_ackno_rxed has been updated in
 * order to clean out wrapped_unacked_segments.
 */
void update_unacked_segment_list(ctcp_state_t *state);

/******************************************************************************
 * Function implementations.
 *****************************************************************************/

ctcp_state_t *ctcp_init(conn_t *conn, ctcp_config_t *cfg) {
  /* Connection could not be established. */
  if (conn == NULL) {
    return NULL;
  }

  /* Established a connection. Create a new state and update the linked list
     of connection states. */
  ctcp_state_t *state = calloc(sizeof(ctcp_state_t), 1);
  state->next = state_list;
  state->prev = &state_list;
  if (state_list)
    state_list->prev = &state->next;
  state_list = state;

  /* Set fields. */
  state->conn = conn;
  /* FIXME: Do any other initialization here. */

  /* Initialize ctcp_config */
  state->ctcp_config.recv_window = cfg->recv_window;
  state->ctcp_config.send_window = cfg->send_window;
  state->ctcp_config.timer = cfg->timer;
  state->ctcp_config.rt_timeout = cfg->rt_timeout;

  /* Initialize tx_state */
  state->tx_state.last_ackno_rxed = 0;
  state->tx_state.has_EOF_been_read = false;
  state->tx_state.last_seqno_read = 0;
  state->tx_state.last_seqno_sent = 0;
  state->tx_state.timestamp_of_last_send = 0;
  /* TODO: DON'T FORGET TO CALL LL_DESTROY()! */
  state->tx_state.wrapped_unacked_segments = ll_create();

  /* Initialize rx_state */
  state->rx_state.last_seqno_accepted = 0;
  state->rx_state.has_FIN_been_rxed = false;
  state->rx_state.num_truncated_segments = 0;
  state->rx_state.num_out_of_order_segments = 0;
  state->rx_state.num_invalid_cksums = 0;
  /* TODO: DON'T FORGET TO CALL LL_DESTROY()! */
  state->rx_state.segments_to_output = ll_create();

  free(cfg);
  return state;
}

void ctcp_destroy(ctcp_state_t *state) {
  unsigned int len, i;
  if (state) {

    // Print any final statistics.
    fprintf(stderr, "state->rx_state.num_truncated_segments:    %u\n",
            state->rx_state.num_truncated_segments);
    fprintf(stderr, "state->rx_state.num_out_of_order_segments: %u\n",
            state->rx_state.num_out_of_order_segments);
    fprintf(stderr, "state->rx_state.num_invalid_cksums:        %u\n",
            state->rx_state.num_invalid_cksums);

    /* Update linked list. */
    if (state->next)
      state->next->prev = state->prev;

    *state->prev = state->next;
    conn_remove(state->conn);

    /* FIXME: Do any other cleanup here. */

    /* Free everything in the list of unacknowledged segments. */
    len = ll_length(state->tx_state.wrapped_unacked_segments);
    if (len) fprintf(stderr, "\n ** UH OH, %d segments were never acknowledged!\n", len);
    for (i = 0; i < len; ++i)
    {
      wrapped_ctcp_segment_t* wrapped_ctcp_segment_ptr;
      ll_node_t *front_node_ptr = ll_front(state->tx_state.wrapped_unacked_segments);
      wrapped_ctcp_segment_ptr = (wrapped_ctcp_segment_t*) front_node_ptr->object;
      print_ctcp_segment(&wrapped_ctcp_segment_ptr->ctcp_segment);
      free(front_node_ptr->object);
      ll_remove(state->tx_state.wrapped_unacked_segments, front_node_ptr);
    }
    ll_destroy(state->tx_state.wrapped_unacked_segments);

    /* Free everything in the list of segments to output. */
    len = ll_length(state->rx_state.segments_to_output);
    if (len) fprintf(stderr, "\n *** UH OH, %d segments were never output!\n", len);
    for (i = 0; i < len; ++i)
    {
      ll_node_t *front_node_ptr = ll_front(state->rx_state.segments_to_output);
      free(front_node_ptr->object);
      ll_remove(state->rx_state.segments_to_output, front_node_ptr);
    }
    ll_destroy(state->rx_state.segments_to_output);

    free(state);
  }
  end_client();
}

void ctcp_read(ctcp_state_t *state) {
  /* FIXME */
  int bytes_read;
  uint8_t buf[MAX_SEG_DATA_SIZE];
  wrapped_ctcp_segment_t* new_segment_ptr;

  if (state->tx_state.has_EOF_been_read)
    return;

  while ((bytes_read = conn_input(state->conn, buf, MAX_SEG_DATA_SIZE)) > 0)
  {
    /* todo remove */
    buf[bytes_read] = 0;
    fprintf(stderr, "Read %d bytes: %s\n", bytes_read, buf);

    /* create a new ctcp segment */
    new_segment_ptr = (wrapped_ctcp_segment_t*) calloc(1,
                                  sizeof(wrapped_ctcp_segment_t) + bytes_read);
    assert(new_segment_ptr != NULL);

    /* Initialize the ctcp segment. Remember that calloc init'd everything to zero.
    ** Most headers should be set by whatever fn actually ships this segment out. */
    new_segment_ptr->ctcp_segment.len = htons((uint16_t) sizeof(ctcp_segment_t) + bytes_read);

    /* Set the segment's sequence number. */
    new_segment_ptr->ctcp_segment.seqno = htonl(state->tx_state.last_seqno_read + 1);

    /* Copy the data we just read into the segment we just allocated. */
    memcpy(new_segment_ptr->ctcp_segment.data, buf, bytes_read);

    /* Set last_seqno_read. Sequence numbers start at 1, not 0, so we don't need
    ** to subtract 1 here. */
    state->tx_state.last_seqno_read += bytes_read;

    /* Add new ctcp segment to our list of unacknowledged segments. */
    ll_add(state->tx_state.wrapped_unacked_segments, new_segment_ptr);
  }

  if (bytes_read == -1)
  {
    state->tx_state.has_EOF_been_read = true;

    /* Create a FIN segment. */
    new_segment_ptr = (wrapped_ctcp_segment_t*) calloc(1, sizeof(wrapped_ctcp_segment_t));
    assert(new_segment_ptr != NULL);
    new_segment_ptr->ctcp_segment.len = htons((uint16_t) sizeof(ctcp_segment_t));
    new_segment_ptr->ctcp_segment.seqno = htonl(state->tx_state.last_seqno_read + 1);
    /* Add new ctcp segment to our list of unacknowledged segments. */
    ll_add(state->tx_state.wrapped_unacked_segments, new_segment_ptr);
    new_segment_ptr->ctcp_segment.flags |= TH_FIN;

    /* TODO REMOVE -- for initial development, end client when we read EOF. */
    fprintf(stderr, "EOF read, ending client\n");
    ctcp_destroy(state_list);
  }

  /* Try to send the data we just read. */
  ctcp_send_what_we_can(state_list);
}

void ctcp_send_what_we_can(ctcp_state_t *state_list) {
  /*
  ** For lab 2, we'll have to walk through state_list and xmit and rexmit
  ** segments. But for lab 1, we're just doing stop_and_wait, so we only
  ** need to send one segment at a time.
  */

  ctcp_state_t *curr_state;
  wrapped_ctcp_segment_t *wrapped_ctcp_segment_ptr;
  ll_node_t *ll_node_ptr;
  long ms_since_last_send;

  if (state_list == NULL)
    return;

  /*
  **  Lab 1 - Stop and Wait
  */
  curr_state = state_list; /* Only one state for lab 1. */

  /* Make sure we actually have something to send. */
  if (ll_length(curr_state->tx_state.wrapped_unacked_segments) == 0)
    return;

  ll_node_ptr = ll_front(curr_state->tx_state.wrapped_unacked_segments);
  wrapped_ctcp_segment_ptr = (wrapped_ctcp_segment_t *) ll_node_ptr->object;

  if (curr_state->tx_state.last_ackno_rxed > curr_state->tx_state.last_seqno_sent) {
    /* The last data byte sent has been acknowledged, so try to send the next segment. */
    ctcp_send_segment(curr_state, wrapped_ctcp_segment_ptr);
  } else {
    /* We're still waiting for an acknowledgment. We might need to retransmit. */
    ms_since_last_send = current_time() - wrapped_ctcp_segment_ptr->timestamp_of_last_send;
    if (ms_since_last_send > curr_state->ctcp_config.rt_timeout) {
      // Timeout. Resend the segment.
      ctcp_send_segment(curr_state, wrapped_ctcp_segment_ptr);
    }
  }
}

void ctcp_send_segment(ctcp_state_t *state, wrapped_ctcp_segment_t* wrapped_segment)
{
  long timestamp;
  uint16_t segment_cksum;
  int bytes_sent;

  if (wrapped_segment->num_xmits >= MAX_NUM_XMITS) {
    // Assume the other side is unresponsive and destroy the connection.
    fprintf(stderr, "xmit limit reached\n");
    ctcp_destroy(state);
    return;
  }

  /* Set the segment's ctcp header fields. */
  wrapped_segment->ctcp_segment.ackno = htonl(state->rx_state.last_seqno_accepted + 1);
  wrapped_segment->ctcp_segment.flags |= TH_ACK;
  wrapped_segment->ctcp_segment.window = htons(state->ctcp_config.recv_window);

  wrapped_segment->ctcp_segment.cksum = 0;
  segment_cksum = cksum(&wrapped_segment->ctcp_segment, ntohs(wrapped_segment->ctcp_segment.len));
  wrapped_segment->ctcp_segment.cksum = segment_cksum;

  /* Try to send the segment. */
  bytes_sent = conn_send(state->conn, &wrapped_segment->ctcp_segment,
                         ntohs(wrapped_segment->ctcp_segment.len));
  timestamp = current_time();

  if (bytes_sent == 0)
    return; // can't send for some reason, try again later.
  if (bytes_sent == -1) {
    fprintf(stderr, "conn_send returned -1.\n");
    ctcp_destroy(state); // ya done now
    return;
  }

  /* Update state */
  state->tx_state.last_seqno_sent += bytes_sent;
  state->tx_state.timestamp_of_last_send = timestamp;
  wrapped_segment->num_xmits++;
  wrapped_segment->timestamp_of_last_send = timestamp;
}


void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {

  uint16_t computed_cksum, actual_cksum, num_data_bytes;

  /* If the segment was truncated, ignore it and hopefully retransmission will fix it. */
  if (len < ntohs(segment->len)) {
    fprintf(stderr, "Ignoring truncated segment.   ");
    print_ctcp_segment(segment);
    free(segment);
    state->rx_state.num_truncated_segments++;
    return;
  }

  num_data_bytes = ntohs(segment->len) - sizeof(ctcp_segment_t);

  /* If the segment arrived out of order, ignore it. */
  if (   (ntohl(segment->seqno) != (state->rx_state.last_seqno_accepted + 1))
      && (num_data_bytes != 0)) {
    fprintf(stderr, "Ignoring out of order segment. ");
    fprintf(stderr, " ntohl(segment->seqno): %d", ntohl(segment->seqno));
    fprintf(stderr, " state->rx_state.last_seqno_accepted + 1: %d\n",
                      state->rx_state.last_seqno_accepted + 1);
    print_ctcp_segment(segment);
    free(segment);
    state->rx_state.num_out_of_order_segments++;
    return;
  }

  // Check the checksum.
  actual_cksum = segment->cksum;
  segment->cksum = 0;
  computed_cksum = cksum(segment, ntohs(segment->len));
  // put it back in case we want to examine the value later
  segment->cksum = actual_cksum;
  if (actual_cksum != computed_cksum)
  {
    fprintf(stderr, "Invalid cksum! Computed=0x%04x, Actual=0x%04x    ",
            computed_cksum, actual_cksum);
    print_ctcp_segment(segment);
    free(segment);
    state->rx_state.num_invalid_cksums++;
    return;
  }

  // todo remove
  fprintf(stderr, "Looks like we got a valid segment with %d bytes\n", num_data_bytes);
  print_ctcp_segment(segment);

  // update rx_state.last_seqno_accepted
  if (num_data_bytes) {
    state->rx_state.last_seqno_accepted += num_data_bytes;
  }

  // if ACK flag is set, update tx_state.last_ackno_rxed
  if (segment->flags & TH_ACK) {
    state->tx_state.last_ackno_rxed = ntohl(segment->ackno);
  }

  // if FIN flag is set, update rx_state.has_FIN_been_rxed
  if (segment->flags & TH_FIN) {
    state->rx_state.has_FIN_been_rxed = true;
    // A FIN should advance the sequence number.
    fprintf(stderr, "received FIN, incrementing state->rx_state.last_seqno_accepted\n");
    state->rx_state.last_seqno_accepted++;
  }

  // append the segment to segments_to_output. We should only output data if
  // the segment has data to output, or if we've received a FIN (in which case
  // we'll need to output EOF.)
  if (num_data_bytes || (segment->flags & TH_FIN))
    ll_add(state->rx_state.segments_to_output, segment);

  // Output as many received segments as we can.
  ctcp_output(state);

  /* The ackno has probably advanced, so clean up our list of unacked segments. */
  update_unacked_segment_list(state);
}

void ctcp_output(ctcp_state_t *state) {

  ll_node_t* front_node_ptr;
  ctcp_segment_t* ctcp_segment_ptr;
  size_t bufspace;
  int num_data_bytes;
  int return_value;
  ctcp_segment_t ctcp_segment;

  if (state == NULL)
    return;

  while (ll_length(state->rx_state.segments_to_output) != 0) {

    // Grab the node we're going to try to output.
    front_node_ptr = ll_front(state->rx_state.segments_to_output);
    ctcp_segment_ptr = (ctcp_segment_t*) front_node_ptr->object;

    num_data_bytes = ntohs(ctcp_segment_ptr->len) - sizeof(ctcp_segment_t);
    // Output any data in this segment.
    if (num_data_bytes) {
      // See if there's enough bufspace right now to output.
      bufspace = conn_bufspace(state->conn);
      if (bufspace < num_data_bytes) {
        // can't send right now, give up and try later.
        return;
      }

      return_value = conn_output(state->conn, ctcp_segment_ptr->data, num_data_bytes);
      if (return_value == -1) {
        fprintf(stderr, "conn_output() returned -1");
        ctcp_destroy(state);
        return;
      }
      assert(return_value == num_data_bytes);
    }

    // If this segment's FIN flag is set, output EOF by setting length to 0.
    if (ctcp_segment_ptr->flags & TH_FIN)
      conn_output(state->conn, ctcp_segment_ptr->data, 0);

    // Send an ack. Acking here (instead of in ctcp_receive) flow controls the
    // sender until buffer space is available.
    ctcp_segment.seqno = htonl(0); // I don't think seqno matters for pure control segments
    ctcp_segment.ackno = htonl(state->rx_state.last_seqno_accepted + 1);
    ctcp_segment.len   = 0;
    ctcp_segment.flags = TH_ACK;
    ctcp_segment.window = htons(state->ctcp_config.recv_window);
    ctcp_segment.cksum = 0;
    ctcp_segment.cksum = cksum(&ctcp_segment, sizeof(ctcp_segment_t));
    // deliberately ignore return value
    conn_send(state->conn, &ctcp_segment, sizeof(ctcp_segment_t));

    // We've successfully output the segment, so remove it from the linked
    // list.
    free(ctcp_segment_ptr);
    ll_remove(state->rx_state.segments_to_output, front_node_ptr);
  }
}

// We'll need to call this after successfully receiving a segment to clean
// acknowledged segments out of wrapped_unacked_segments
void update_unacked_segment_list(ctcp_state_t *state) {
  ll_node_t* front_node_ptr;
  wrapped_ctcp_segment_t* wrapped_ctcp_segment_ptr;
  uint32_t seqno_of_last_byte;
  uint16_t num_data_bytes;

  while (ll_length(state->tx_state.wrapped_unacked_segments) != 0) {
    front_node_ptr = ll_front(state->tx_state.wrapped_unacked_segments);
    wrapped_ctcp_segment_ptr = (wrapped_ctcp_segment_t*) front_node_ptr->object;
    num_data_bytes = ntohs(wrapped_ctcp_segment_ptr->ctcp_segment.len) - sizeof(ctcp_segment_t);
    seqno_of_last_byte =   ntohl(wrapped_ctcp_segment_ptr->ctcp_segment.seqno)
                         + num_data_bytes - 1;

    if (seqno_of_last_byte < state->tx_state.last_ackno_rxed) {
      // This segment has been acknowledged.
      fprintf(stderr,
              "Cleaning out acknowledged segment with seqno_of_last_byte: %d\n",
              seqno_of_last_byte);
      free(wrapped_ctcp_segment_ptr);
      ll_remove(state->tx_state.wrapped_unacked_segments, front_node_ptr);
    } else {
      // This segment has not been acknowledged, so our cleanup is done.
      return;
    }
  }
}

void ctcp_timer() {

  ctcp_state_t * curr_state;

  /*
  ** For lab 1 stop and wait, we only have to worry about one connection.
  */
  curr_state = state_list;

  /* FIXME */
  ctcp_send_what_we_can(curr_state);
  ctcp_output(curr_state);

  /*
  ** TODO - walk through data model and make sure I'm updating everything
  */
}
