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

/**
 * ctcp_state child structs
 */
typedef struct {
  uint32_t last_ackno_rxed;
  bool has_my_FIN_been_sent; /* Todo - not sure if necessary */
  bool has_my_FIN_been_acked;
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
  state->tx_state.has_my_FIN_been_sent = false;
  state->tx_state.has_my_FIN_been_acked = false;
  state->tx_state.has_EOF_been_read = false;
  state->tx_state.last_seqno_read = 0;
  state->tx_state.last_seqno_sent = 0;
  state->tx_state.timestamp_of_last_send = 0;
  /* TODO: DON'T FORGET TO CALL LL_DESTROY()! */
  state->tx_state.wrapped_unacked_segments = ll_create();

  /* Initialize rx_state */
  state->rx_state.last_seqno_accepted = 0;
  state->rx_state.has_FIN_been_rxed = false;
  /* TODO: DON'T FORGET TO CALL LL_DESTROY()! */
  state->rx_state.segments_to_output = ll_create();

  free(cfg);
  return state;
}

void ctcp_destroy(ctcp_state_t *state) {
  unsigned int len, i;
  if (state) {
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
      ll_node_t *front_node_ptr = ll_front(state->tx_state.wrapped_unacked_segments);
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

  while ((bytes_read = conn_input(state->conn, buf, MAX_SEG_DATA_SIZE)) > 0)
  {
    /* todo remove */
    buf[bytes_read] = 0;
    fprintf(stderr, "Read %d bytes: %s\n", bytes_read, buf);

    /* create a new ctcp segment */
    new_segment_ptr = (wrapped_ctcp_segment_t*) calloc(1,
                                  sizeof(wrapped_ctcp_segment_t) + bytes_read);
    assert(new_segment_ptr != NULL);

    /* Initialize the ctcp segment */
    new_segment_ptr->num_xmits = 0;
    new_segment_ptr->timestamp_of_last_send = 0;
    new_segment_ptr->ctcp_segment.seqno = 0;

    /* todo - initialize other members*/
    /* DONT FORGET TO ALLOCATE DATA, AND FREE IT IN CTCP_DESTROY! */

    /* Add new ctcp segment to our list of unacknowledged segments. */
    ll_add(state->tx_state.wrapped_unacked_segments, new_segment_ptr);

    /* TODO: At this point, I'm thinking there should be a utility function
    ** called maybe 'update_state_and_send_segment()' which will take a
    ** pointer to a segment (e.g., new_segment_ptr), and update state as well
    ** as the ctcp_segment header stuff.
    */
  }

  /* TODO: handle the case when bytes_read == 1 */

}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {
  /* FIXME */
}

void ctcp_output(ctcp_state_t *state) {
  /* FIXME */
}

#define TIMEOUT_IN_MS 5000
long time_of_first_call = 0; /* todo remove */
void ctcp_timer() {
/*  static long time_of_first_call = current_time();*/
  if (time_of_first_call == 0) time_of_first_call = current_time();

  /* Simple cleanup for initial development: After this many milliseconds, call
  ** ctcp_destroy(). Get rid of this eventually.  */
  if (current_time() - time_of_first_call > TIMEOUT_IN_MS)
  {
    ctcp_destroy(state_list);
  }
}
