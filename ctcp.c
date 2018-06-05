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

#undef ENABLE_DBG_PRINTS

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

  /* Make this point to a wrapped_ctcp_segment_t:
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
  uint32_t num_out_of_window_segments;
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

  /* This will be set when we first detect that we should close down a
  ** connection, so that we can wait twice the maximum segment lifetime before
  ** actually closing down the connection. */
  long FIN_WAIT_start_time;

  ctcp_config_t ctcp_config;
  tx_state_t tx_state;
  rx_state_t rx_state;
};

/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;

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
 * order to clean out wrapped_unacked_segments that have now been acked.
 */
void ctcp_clean_up_unacked_segment_list(ctcp_state_t *state);

/**
 * Send a cTCP segment with no data, so that we can inform whoever's on the
 * other end of the connection of our current state.
 */
void ctcp_send_control_segment(ctcp_state_t *state);

/**
 * Returns the number of data bytes (i.e., non-header bytes) in the segment.
 */
uint16_t ctcp_get_num_data_bytes(ctcp_segment_t* ctcp_segment_ptr);

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

  state->FIN_WAIT_start_time = 0;

  /* Initialize ctcp_config */
  state->ctcp_config.recv_window = cfg->recv_window;
  state->ctcp_config.send_window = cfg->send_window;
  state->ctcp_config.timer = cfg->timer;
  state->ctcp_config.rt_timeout = cfg->rt_timeout;

  #ifdef ENABLE_DBG_PRINTS
  fprintf(stderr, "state->ctcp_config.recv_window  : %d\n", state->ctcp_config.recv_window );
  fprintf(stderr, "state->ctcp_config.send_window  : %d\n", state->ctcp_config.send_window );
  fprintf(stderr, "state->ctcp_config.timer        : %d\n", state->ctcp_config.timer );
  fprintf(stderr, "state->ctcp_config.rt_timeout   : %d\n", state->ctcp_config.rt_timeout );
  #endif

  /* Initialize tx_state */
  state->tx_state.last_ackno_rxed = 0;
  state->tx_state.has_EOF_been_read = false;
  state->tx_state.last_seqno_read = 0;
  state->tx_state.last_seqno_sent = 0;
  state->tx_state.wrapped_unacked_segments = ll_create();

  /* Initialize rx_state */
  state->rx_state.last_seqno_accepted = 0;
  state->rx_state.has_FIN_been_rxed = false;
  state->rx_state.num_truncated_segments = 0;
  state->rx_state.num_out_of_window_segments = 0;
  state->rx_state.num_invalid_cksums = 0;
  state->rx_state.segments_to_output = ll_create();

  free(cfg);
  return state;
}

void ctcp_destroy(ctcp_state_t *state) {
  unsigned int len, i;
  if (state) {

    // Print any final statistics.
    #ifdef ENABLE_DBG_PRINTS
    fprintf(stderr, "state->rx_state.num_truncated_segments:    %u\n",
            state->rx_state.num_truncated_segments);
    fprintf(stderr, "state->rx_state.num_out_of_window_segments: %u\n",
            state->rx_state.num_out_of_window_segments);
    fprintf(stderr, "state->rx_state.num_invalid_cksums:        %u\n",
            state->rx_state.num_invalid_cksums);
    #endif

    /* Update linked list. */
    if (state->next)
      state->next->prev = state->prev;

    *state->prev = state->next;
    conn_remove(state->conn);

    /* FIXME: Do any other cleanup here. */

    /* Free everything in the list of unacknowledged segments. */
    len = ll_length(state->tx_state.wrapped_unacked_segments);
    #ifdef ENABLE_DBG_PRINTS
    if (len) fprintf(stderr, "\n ** UH OH, %d segments were never acknowledged!\n", len);
    #endif
    for (i = 0; i < len; ++i)
    {
      ll_node_t *front_node_ptr = ll_front(state->tx_state.wrapped_unacked_segments);
      #ifdef ENABLE_DBG_PRINTS
      wrapped_ctcp_segment_t* wrapped_ctcp_segment_ptr =
                              (wrapped_ctcp_segment_t*) front_node_ptr->object;
      print_ctcp_segment(&wrapped_ctcp_segment_ptr->ctcp_segment);
      #endif
      free(front_node_ptr->object);
      ll_remove(state->tx_state.wrapped_unacked_segments, front_node_ptr);
    }
    ll_destroy(state->tx_state.wrapped_unacked_segments);

    /* Free everything in the list of segments to output. */
    len = ll_length(state->rx_state.segments_to_output);
    #ifdef ENABLE_DBG_PRINTS
    if (len) fprintf(stderr, "\n *** UH OH, %d segments were never output!\n", len);
    #endif
    for (i = 0; i < len; ++i)
    {
      ll_node_t *front_node_ptr = ll_front(state->rx_state.segments_to_output);
      free(front_node_ptr->object);
      ll_remove(state->rx_state.segments_to_output, front_node_ptr);
    }
    ll_destroy(state->rx_state.segments_to_output);

    free(state);
  }
  #ifdef ENABLE_DBG_PRINTS
  // fprintf(stderr, "\n\nStarting infinite loop...\n\n");
  // while (1); // Loop forever to allow output to be seen.
  #endif
  end_client();
}

void ctcp_read(ctcp_state_t *state) {
  /* FIXME */
  int bytes_read;
  // I add 1 here so that I can add null terminator in debug code
  uint8_t buf[MAX_SEG_DATA_SIZE+1];
  wrapped_ctcp_segment_t* new_segment_ptr;

  if (state->tx_state.has_EOF_been_read)
    return;

  while ((bytes_read = conn_input(state->conn, buf, MAX_SEG_DATA_SIZE)) > 0)
  {
    #ifdef ENABLE_DBG_PRINTS
    buf[bytes_read] = 0; // add null terminator
    fprintf(stderr, "Read %d bytes: %s\n", bytes_read, buf);
    #endif

    /*
    ** Create a new ctcp segment.
    **
    ** An implementation that would lead to less memory fragmentation
    ** would be to allocate consistent sized blocks. Not sure if I'll have
    ** time for that.
    */
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
    new_segment_ptr->ctcp_segment.flags |= TH_FIN;
    /* Add new ctcp segment to our list of unacknowledged segments. */
    ll_add(state->tx_state.wrapped_unacked_segments, new_segment_ptr);
  }

  /* Try to send the data we just read. */
  ctcp_send_what_we_can(state_list);
}

void ctcp_send_what_we_can(ctcp_state_t *state_list) {

  ctcp_state_t *curr_state;
  wrapped_ctcp_segment_t *wrapped_ctcp_segment_ptr;
  ll_node_t *curr_node_ptr;
  long ms_since_last_send;
  unsigned int i, length;
  uint32_t last_seqno_of_segment, last_allowable_seqno;

  if (state_list == NULL)
    return;

  curr_state = state_list; /* todo - implement multiple connections later */
  length = ll_length(curr_state->tx_state.wrapped_unacked_segments);
  if (length == 0)
    /* todo - this will have to be 'continue' or something for multiple connections */
    return;

  for (i = 0; i < length; ++i) {
    if (i == 0) {
      curr_node_ptr = ll_front(curr_state->tx_state.wrapped_unacked_segments);
    } else {
      curr_node_ptr = curr_node_ptr->next;
    }

    wrapped_ctcp_segment_ptr = (wrapped_ctcp_segment_t *) curr_node_ptr->object;

    // Empty segments shouldn't make it into wrapped_unacked_segments.
    assert (wrapped_ctcp_segment_ptr->ctcp_segment.len != 0);

    last_seqno_of_segment = ntohl(wrapped_ctcp_segment_ptr->ctcp_segment.seqno)
      + ctcp_get_num_data_bytes(&wrapped_ctcp_segment_ptr->ctcp_segment) - 1;

    // Subtract 1 because the ackno is byte they want next, not the last byte
    // they've received.
    last_allowable_seqno = curr_state->tx_state.last_ackno_rxed - 1
      + curr_state->ctcp_config.send_window;

    if (curr_state->tx_state.last_ackno_rxed == 0) {
      ++last_allowable_seqno; // last_ackno_rxed starts at 0
    }

    // If the segment is outside of the sliding window, then we're done.
    // "maintain invariant (LSS-LAR <= SWS)"
    if (last_seqno_of_segment > last_allowable_seqno) {
      return;
    }

    // If we got to this point, then we have a segment that's within the send
    // window. Any segments here that have not been sent can now be sent. The
    // first segment can be retransmitted if it timed out.
    if (wrapped_ctcp_segment_ptr->num_xmits == 0) {
      ctcp_send_segment(curr_state, wrapped_ctcp_segment_ptr);
    } else if (i == 0) {
      // Check and see if we need to retrasnmit the first segment.
      ms_since_last_send = current_time() - wrapped_ctcp_segment_ptr->timestamp_of_last_send;
      if (ms_since_last_send > curr_state->ctcp_config.rt_timeout) {
        // Timeout. Resend the segment.
        ctcp_send_segment(curr_state, wrapped_ctcp_segment_ptr);
      }
    }
  }


#if 0
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
#endif
}

void ctcp_send_segment(ctcp_state_t *state, wrapped_ctcp_segment_t* wrapped_segment)
{
  long timestamp;
  uint16_t segment_cksum;
  int bytes_sent;

  if (wrapped_segment->num_xmits >= MAX_NUM_XMITS) {
    // Assume the other side is unresponsive and destroy the connection.
    #ifdef ENABLE_DBG_PRINTS
    fprintf(stderr, "xmit limit reached\n");
    #endif
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
  wrapped_segment->num_xmits++;

  /*if (bytes_sent == 0)*/
  if (bytes_sent < ntohs(wrapped_segment->ctcp_segment.len) ) {
    #ifdef ENABLE_DBG_PRINTS
    fprintf(stderr, "conn_send returned %d bytes instead of %d :-(\n",
            bytes_sent, ntohs(wrapped_segment->ctcp_segment.len));
    #endif
    return; // can't send for some reason, try again later.
  }
  if (bytes_sent == -1) {
    #ifdef ENABLE_DBG_PRINTS
    fprintf(stderr, "conn_send returned -1.\n");
    #endif
    ctcp_destroy(state); // ya done now
    return;
  }

  #ifdef ENABLE_DBG_PRINTS
  fprintf(stderr, "SENT  ");
  print_ctcp_segment(&wrapped_segment->ctcp_segment);
  #endif

  /* Update state */
  state->tx_state.last_seqno_sent += bytes_sent;
  wrapped_segment->timestamp_of_last_send = timestamp;
}


void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {

  uint16_t computed_cksum, actual_cksum, num_data_bytes;
  uint32_t last_seqno_of_segment, largest_allowable_seqno, smallest_allowable_seqno;
  unsigned int length, i;
  ll_node_t* ll_node_ptr;
  ctcp_segment_t* ctcp_segment_ptr;

  /* If the segment was truncated, ignore it and hopefully retransmission will fix it. */
  if (len < ntohs(segment->len)) {
    #ifdef ENABLE_DBG_PRINTS
    fprintf(stderr, "Ignoring truncated segment.   ");
    print_ctcp_segment(segment);
    #endif
    free(segment);
    state->rx_state.num_truncated_segments++;
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
    #ifdef ENABLE_DBG_PRINTS
    fprintf(stderr, "Invalid cksum! Computed=0x%04x, Actual=0x%04x    ",
            computed_cksum, actual_cksum);
    print_ctcp_segment(segment);
    #endif
    free(segment);
    state->rx_state.num_invalid_cksums++;
    return;
  }

  num_data_bytes = ntohs(segment->len) - sizeof(ctcp_segment_t);

  // Reject the segment if it's outside of the receive window.
  if (num_data_bytes) {
    last_seqno_of_segment = ntohl(segment->seqno) + num_data_bytes - 1;
    smallest_allowable_seqno = state->rx_state.last_seqno_accepted + 1;
    largest_allowable_seqno = state->rx_state.last_seqno_accepted
      + state->ctcp_config.recv_window;

    if ((last_seqno_of_segment > largest_allowable_seqno) ||
        (ntohl(segment->seqno) < smallest_allowable_seqno)) {
      #ifdef  ENABLE_DBG_PRINTS
      fprintf(stderr, "Ignoring out of window segment. ");
      print_ctcp_segment(segment);
      #endif
      free(segment);
      // Let the sender know our state, since they sent a wonky packet. Maybe
      // our previous ack was lost.
      ctcp_send_control_segment(state);
      state->rx_state.num_out_of_window_segments++;
      return;
    }
  }

/* Lab 1 */
#if 0
  /* If the segment arrived out of order, ignore it. */
  if (   (ntohl(segment->seqno) != (state->rx_state.last_seqno_accepted + 1))
      && (num_data_bytes != 0)) {
    #ifdef ENABLE_DBG_PRINTS
    fprintf(stderr, "Ignoring out of order segment. ");
    fprintf(stderr, " ntohl(segment->seqno): %d", ntohl(segment->seqno));
    fprintf(stderr, " state->rx_state.last_seqno_accepted + 1: %d\n",
                      state->rx_state.last_seqno_accepted + 1);
    print_ctcp_segment(segment);
    #endif
    free(segment);

    state->rx_state.num_out_of_order_segments++;

    // Our 'ACK' of the out of order segment might have been lost, so send it
    // again.
    ctcp_send_control_segment(state);
    return;
  }
#endif

  #ifdef ENABLE_DBG_PRINTS
  fprintf(stderr, "Looks like we got a valid segment with %d bytes\n", num_data_bytes);
  print_ctcp_segment(segment);
  #endif

  // if ACK flag is set, update tx_state.last_ackno_rxed
  if (segment->flags & TH_ACK) {
    state->tx_state.last_ackno_rxed = ntohl(segment->ackno);
  }


  /*
  ** Try to add the segment to segments_to_output. We should only output data if
  ** the segment has data to output, or if we've received a FIN (in which case
  ** we'll need to output EOF.)
  */
  if (num_data_bytes || (segment->flags & TH_FIN))
  {
    /*
    ** We need to add the segment to the linked list segments_to_output in
    ** sorted order, taking care to throw away/free it if it's a duplicate.
    */
    length = ll_length(state->rx_state.segments_to_output);

    if (length == 0)
    {
      ll_add(state->rx_state.segments_to_output, segment);
    }
    else if (length == 1)
    {
      ll_node_ptr = ll_front(state->rx_state.segments_to_output);
      ctcp_segment_ptr = (ctcp_segment_t*) ll_node_ptr->object;
      if (ntohl(segment->seqno) == ntohl(ctcp_segment_ptr->seqno))
      {
        // The segment we received is a duplicate, so throw it away.
        free(segment);
      }
      else if (ntohl(segment->seqno) > ntohl(ctcp_segment_ptr->seqno))
      {
        // the new segment comes after the one segment we have
        ll_add(state->rx_state.segments_to_output, segment);
      }
      else
      {
        // the new segment comes earlier than the one segment we have
        ll_add_front(state->rx_state.segments_to_output, segment);
      }
    }
    else
    {
      // We have at least two nodes, and need to figure out what to do with the
      // new segment.
      ctcp_segment_t* first_ctcp_segment_ptr;
      ctcp_segment_t* last_ctcp_segment_ptr;
      ll_node_t* first_ll_node_ptr;
      ll_node_t* last_ll_node_ptr;

      first_ll_node_ptr = ll_front(state->rx_state.segments_to_output);
      last_ll_node_ptr  = ll_back(state->rx_state.segments_to_output);

      first_ctcp_segment_ptr = (ctcp_segment_t*) first_ll_node_ptr->object;
      last_ctcp_segment_ptr  = (ctcp_segment_t*) last_ll_node_ptr->object;

      // See if we should add the segment to the end of the list.
      if (ntohl(segment->seqno) > ntohl(last_ctcp_segment_ptr->seqno))
      {
        ll_add(state->rx_state.segments_to_output, segment);
      }
      // See if we should add the segment to the beginning of the list.
      else if (ntohl(segment->seqno) < ntohl(first_ctcp_segment_ptr->seqno))
      {
        ll_add_front(state->rx_state.segments_to_output, segment);
      }
      // The segment is either a duplicate, or it belongs *between* two nodes.
      else
      {
        for (i = 0; i < (length-1); ++i) // for every *pair* of nodes
        {
          ll_node_t* curr_node_ptr;
          ll_node_t* next_node_ptr;

          ctcp_segment_t* curr_ctcp_segment_ptr;
          ctcp_segment_t* next_ctcp_segment_ptr;

          if (i == 0) {
            curr_node_ptr = ll_front(state->rx_state.segments_to_output);
          } else {
            curr_node_ptr = curr_node_ptr->next;
          }
          next_node_ptr = curr_node_ptr->next;

          curr_ctcp_segment_ptr = (ctcp_segment_t*) curr_node_ptr->object;
          next_ctcp_segment_ptr = (ctcp_segment_t*) next_node_ptr->object;

          // Check for duplicates.
          if ((ntohl(segment->seqno) == ntohl(curr_ctcp_segment_ptr->seqno)) ||
              (ntohl(segment->seqno) == ntohl(next_ctcp_segment_ptr->seqno)))
          {
            // Duplicate found.
            free(segment);
            break;
          }
          else
          {
            // See if we can add the node between the two nodes.
            if ((ntohl(segment->seqno) > ntohl(curr_ctcp_segment_ptr->seqno)) &&
                (ntohl(segment->seqno) < ntohl(next_ctcp_segment_ptr->seqno)))
            {
              ll_add_after(state->rx_state.segments_to_output, curr_node_ptr, segment);
              break;
            }
          }
        } /* End of  `for (i = 0; i < length; ++i) `  */
      }
    }

  } /* End of  `if (num_data_bytes || (segment->flags & TH_FIN))`    */
  else
  {
    // Segment contains no data, so don't update the linked list. We've updated
    // our state at this point and can free the segment.
    free(segment);
  }

  // Output as many received segments as we can.
  ctcp_output(state);

  /* The ackno has probably advanced, so clean up our list of unacked segments. */
  ctcp_clean_up_unacked_segment_list(state);
}

void ctcp_output(ctcp_state_t *state) {

  ll_node_t* front_node_ptr;
  ctcp_segment_t* ctcp_segment_ptr;
  size_t bufspace;
  int num_data_bytes;
  int return_value;
  int num_segments_output = 0;

  if (state == NULL)
    return;

  while (ll_length(state->rx_state.segments_to_output) != 0) {

    // Grab the segment we're going to try to output.
    front_node_ptr = ll_front(state->rx_state.segments_to_output);
    ctcp_segment_ptr = (ctcp_segment_t*) front_node_ptr->object;

    num_data_bytes = ntohs(ctcp_segment_ptr->len) - sizeof(ctcp_segment_t);
    // Output any data in this segment.
    if (num_data_bytes) {

      // Check the segment's sequence number. There might be a hole in
      // segments_to_output, in which case we should give up.
      if ( ntohl(ctcp_segment_ptr->seqno) != state->rx_state.last_seqno_accepted + 1)
      {
        return;
      }

      // See if there's enough bufspace right now to output.
      bufspace = conn_bufspace(state->conn);
      if (bufspace < num_data_bytes) {
        // can't send right now, give up and try later.
        return;
      }

      return_value = conn_output(state->conn, ctcp_segment_ptr->data, num_data_bytes);
      if (return_value == -1) {
        #ifdef ENABLE_DBG_PRINTS
        fprintf(stderr, "conn_output() returned -1\n");
        #endif

        ctcp_destroy(state);
        return;
      }
      assert(return_value == num_data_bytes);
      num_segments_output++;
    }

    // update rx_state.last_seqno_accepted
    if (num_data_bytes) {
      state->rx_state.last_seqno_accepted += num_data_bytes;
    }

    // If this segment's FIN flag is set, output EOF by setting length to 0,
    // and update state.
    if ((!state->rx_state.has_FIN_been_rxed) && (ctcp_segment_ptr->flags & TH_FIN)) {
      state->rx_state.has_FIN_been_rxed = true;
      #ifdef ENABLE_DBG_PRINTS
      fprintf(stderr, "received FIN, incrementing state->rx_state.last_seqno_accepted\n");
      #endif
      state->rx_state.last_seqno_accepted++;
      conn_output(state->conn, ctcp_segment_ptr->data, 0);
      num_segments_output++;
    }

    // We've successfully output the segment, so remove it from the linked
    // list.
    free(ctcp_segment_ptr);
    ll_remove(state->rx_state.segments_to_output, front_node_ptr);
  }

  if (num_segments_output) {
    // Send an ack. Acking here (instead of in ctcp_receive) flow controls the
    // sender until buffer space is available.
    ctcp_send_control_segment(state);
  }
}

// We'll need to call this after successfully receiving a segment to clean
// acknowledged segments out of wrapped_unacked_segments
void ctcp_clean_up_unacked_segment_list(ctcp_state_t *state) {
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
      #ifdef ENABLE_DBG_PRINTS
      fprintf(stderr,
              "Cleaning out acknowledged segment with seqno_of_last_byte: %d\n",
              seqno_of_last_byte);
      #endif
      free(wrapped_ctcp_segment_ptr);
      ll_remove(state->tx_state.wrapped_unacked_segments, front_node_ptr);
    } else {
      // This segment has not been acknowledged, so our cleanup is done.
      return;
    }
  }
}

void ctcp_send_control_segment(ctcp_state_t *state) {
  ctcp_segment_t ctcp_segment;

  ctcp_segment.seqno = htonl(0); // I don't think seqno matters for pure control segments
  ctcp_segment.ackno = htonl(state->rx_state.last_seqno_accepted + 1);
  ctcp_segment.len   = sizeof(ctcp_segment_t);
  ctcp_segment.flags = TH_ACK;
  ctcp_segment.window = htons(state->ctcp_config.recv_window);
  ctcp_segment.cksum = 0;
  ctcp_segment.cksum = cksum(&ctcp_segment, sizeof(ctcp_segment_t));

  // deliberately ignore return value
  conn_send(state->conn, &ctcp_segment, sizeof(ctcp_segment_t));
}

uint16_t ctcp_get_num_data_bytes(ctcp_segment_t* ctcp_segment_ptr)
{
  return ntohs(ctcp_segment_ptr->len) - sizeof(ctcp_segment_t);
}

void ctcp_timer() {

  ctcp_state_t * curr_state;

  if (state_list == NULL) return;

  /*
  ** For lab 1 stop and wait, we only have to worry about one connection.
  */
  curr_state = state_list;

  ctcp_output(curr_state);
  ctcp_send_what_we_can(curr_state);

  /* See if we need close down the connection. We can do this if:
   *   - FIN has been received from the other end (i.e., they have no more data
   *     to send us)
   *   - EOF has been read (i.e., user has no more data to send)
   *   - wrapped_unacked_segments is empty (i.e., all data we've sent
   *     (including the final FIN) has been acked)
   *   - segments_to_output is empty (i.e., we've nothing more to output)
   */

  if (   (curr_state->rx_state.has_FIN_been_rxed)
      && (curr_state->tx_state.has_EOF_been_read)
      && (ll_length(curr_state->tx_state.wrapped_unacked_segments) == 0)
      && (ll_length(curr_state->rx_state.segments_to_output) == 0)) {

    // Wait twice the maximum segment lifetime before tearing down the connection.
    if (curr_state->FIN_WAIT_start_time == 0) {
      #ifdef ENABLE_DBG_PRINTS
      fprintf(stderr, "Closing down connection after 2xMSL...");
      #endif
      curr_state->FIN_WAIT_start_time = current_time();
    } else if ((current_time() - curr_state->FIN_WAIT_start_time) > (2*MAX_SEG_LIFETIME_MS)) {
      #ifdef ENABLE_DBG_PRINTS
      fprintf(stderr, "now closing down the connection.\n");
      #endif
      ctcp_destroy(curr_state);
    }
  }
}
