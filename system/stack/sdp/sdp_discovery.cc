/******************************************************************************
 *
 *  Copyright 1999-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  this file contains SDP discovery functions
 *
 ******************************************************************************/

#define LOG_TAG "sdp_discovery"

#include <bluetooth/log.h>

#include <cstdint>

#include "internal_include/bt_target.h"
#include "osi/include/allocator.h"
#include "stack/include/bt_hdr.h"
#include "stack/include/bt_types.h"
#include "stack/include/sdpdefs.h"
#include "stack/sdp/sdp_discovery_db.h"
#include "stack/sdp/sdpint.h"
#include "types/bluetooth/uuid.h"
#include "types/raw_address.h"

using bluetooth::Uuid;
using namespace bluetooth;

/* Safety check in case we go crazy */
#define MAX_NEST_LEVELS 5

/*******************************************************************************
 *
 * Function         sdpu_build_uuid_seq
 *
 * Description      This function builds a UUID sequence from the list of
 *                  passed UUIDs. It is also passed the address of the output
 *                  buffer.
 *
 * Returns          Pointer to next byte in the output buffer.
 *
 ******************************************************************************/
static uint8_t* sdpu_build_uuid_seq(uint8_t* p_out, uint16_t num_uuids,
                                    Uuid* p_uuid_list, uint16_t& bytes_left) {
  uint16_t xx;
  uint8_t* p_len;

  if (bytes_left < 2) {
    DCHECK(0) << "SDP: No space for data element header";
    return (p_out);
  }

  /* First thing is the data element header */
  UINT8_TO_BE_STREAM(p_out, (DATA_ELE_SEQ_DESC_TYPE << 3) | SIZE_IN_NEXT_BYTE);

  /* Remember where the length goes. Leave space for it. */
  p_len = p_out;
  p_out += 1;

  /* Account for data element header and length */
  bytes_left -= 2;

  /* Now, loop through and put in all the UUID(s) */
  for (xx = 0; xx < num_uuids; xx++, p_uuid_list++) {
    int len = p_uuid_list->GetShortestRepresentationSize();

    if (len + 1 > bytes_left) {
      DCHECK(0) << "SDP: Too many UUIDs for internal buffer";
      break;
    } else {
      bytes_left -= (len + 1);
    }

    if (len == Uuid::kNumBytes16) {
      UINT8_TO_BE_STREAM(p_out, (UUID_DESC_TYPE << 3) | SIZE_TWO_BYTES);
      UINT16_TO_BE_STREAM(p_out, p_uuid_list->As16Bit());
    } else if (len == Uuid::kNumBytes32) {
      UINT8_TO_BE_STREAM(p_out, (UUID_DESC_TYPE << 3) | SIZE_FOUR_BYTES);
      UINT32_TO_BE_STREAM(p_out, p_uuid_list->As32Bit());
    } else if (len == Uuid::kNumBytes128) {
      UINT8_TO_BE_STREAM(p_out, (UUID_DESC_TYPE << 3) | SIZE_SIXTEEN_BYTES);
      ARRAY_TO_BE_STREAM(p_out, p_uuid_list->To128BitBE(),
                         (int)Uuid::kNumBytes128);
    } else {
      DCHECK(0) << "SDP: Passed UUID has invalid length " << len;
    }
  }

  /* Now, put in the length */
  xx = (uint16_t)(p_out - p_len - 1);
  UINT8_TO_BE_STREAM(p_len, xx);

  return (p_out);
}

/*******************************************************************************
 *
 * Function         sdp_snd_service_search_req
 *
 * Description      Send a service search request to the SDP server.
 *
 * Returns          void
 *
 ******************************************************************************/
static void sdp_snd_service_search_req(tCONN_CB* p_ccb, uint8_t cont_len,
                                       uint8_t* p_cont) {
  uint8_t *p, *p_start, *p_param_len;
  BT_HDR* p_cmd = (BT_HDR*)osi_malloc(SDP_DATA_BUF_SIZE);
  uint16_t param_len;
  uint16_t bytes_left = SDP_DATA_BUF_SIZE;

  /* Prepare the buffer for sending the packet to L2CAP */
  p_cmd->offset = L2CAP_MIN_OFFSET;
  p = p_start = (uint8_t*)(p_cmd + 1) + L2CAP_MIN_OFFSET;

  /* Build a service search request packet */
  UINT8_TO_BE_STREAM(p, SDP_PDU_SERVICE_SEARCH_REQ);
  UINT16_TO_BE_STREAM(p, p_ccb->transaction_id);
  p_ccb->transaction_id++;

  /* Skip the length, we need to add it at the end */
  p_param_len = p;
  p += 2;

  /* Account for header size, max service record count and
   * continuation state */
  const uint16_t base_bytes = (sizeof(BT_HDR) + L2CAP_MIN_OFFSET +
                               3u + /* service search request header */
                               2u + /* param len */
                               3u + ((p_cont) ? cont_len : 0));

  if (base_bytes > bytes_left) {
    DCHECK(0) << "SDP: Overran SDP data buffer";
    osi_free(p_cmd);
    return;
  }

  bytes_left -= base_bytes;

  /* Build the UID sequence. */
  p = sdpu_build_uuid_seq(p, p_ccb->p_db->num_uuid_filters,
                          p_ccb->p_db->uuid_filters, bytes_left);

  /* Set max service record count */
  UINT16_TO_BE_STREAM(p, sdp_cb.max_recs_per_search);

  /* Set continuation state */
  UINT8_TO_BE_STREAM(p, cont_len);

  /* if this is not the first request */
  if (cont_len && p_cont) {
    memcpy(p, p_cont, cont_len);
    p += cont_len;
  }

  /* Go back and put the parameter length into the buffer */
  param_len = (uint16_t)(p - p_param_len - 2);
  UINT16_TO_BE_STREAM(p_param_len, param_len);

  p_ccb->disc_state = SDP_DISC_WAIT_HANDLES;

  /* Set the length of the SDP data in the buffer */
  p_cmd->len = (uint16_t)(p - p_start);

  if (L2CA_DataWrite(p_ccb->connection_id, p_cmd) != L2CAP_DW_SUCCESS) {
    log::warn("Unable to write L2CAP data peer:{} cid:{} len:{}",
              p_ccb->device_address, p_ccb->connection_id, p - p_start);
  }

  /* Start inactivity timer */
  alarm_set_on_mloop(p_ccb->sdp_conn_timer, SDP_INACT_TIMEOUT_MS,
                     sdp_conn_timer_timeout, p_ccb);
}

/*******************************************************************************
 *
 * Function         sdp_copy_raw_data
 *
 * Description      copy the raw data
 *
 *
 * Returns          bool
 *                          true if successful
 *                          false if not copied
 *
 ******************************************************************************/
static bool sdp_copy_raw_data(tCONN_CB* p_ccb, bool offset) {
  unsigned int cpy_len, rem_len;
  uint32_t list_len;
  uint8_t* p;
  uint8_t* p_end;
  uint8_t type;

  if (p_ccb->p_db && p_ccb->p_db->raw_data) {
    cpy_len = p_ccb->p_db->raw_size - p_ccb->p_db->raw_used;
    list_len = p_ccb->list_len;
    p = &p_ccb->rsp_list[0];
    p_end = &p_ccb->rsp_list[0] + list_len;

    if (offset) {
      cpy_len -= 1;
      type = *p++;
      uint8_t* old_p = p;
      p = sdpu_get_len_from_type(p, p_end, type, &list_len);
      if (p == NULL || (p + list_len) > p_end) {
        log::warn("bad length");
        return false;
      }
      if ((int)cpy_len < (p - old_p)) {
        log::warn("no bytes left for data");
        return false;
      }
      cpy_len -= (p - old_p);
    }
    if (list_len < cpy_len) {
      cpy_len = list_len;
    }
    rem_len = SDP_MAX_LIST_BYTE_COUNT - (unsigned int)(p - &p_ccb->rsp_list[0]);
    if (cpy_len > rem_len) {
      log::warn("rem_len :{} less than cpy_len:{}", rem_len, cpy_len);
      cpy_len = rem_len;
    }
    memcpy(&p_ccb->p_db->raw_data[p_ccb->p_db->raw_used], p, cpy_len);
    p_ccb->p_db->raw_used += cpy_len;
  }
  return true;
}

/*******************************************************************************
 *
 * Function         add_record
 *
 * Description      This function allocates space for a record from the DB.
 *
 * Returns          pointer to next byte in data stream
 *
 ******************************************************************************/
tSDP_DISC_REC* add_record(tSDP_DISCOVERY_DB* p_db, const RawAddress& bd_addr) {
  tSDP_DISC_REC* p_rec;

  /* See if there is enough space in the database */
  if (p_db->mem_free < sizeof(tSDP_DISC_REC)) return (NULL);

  p_rec = (tSDP_DISC_REC*)p_db->p_free_mem;
  p_db->p_free_mem += sizeof(tSDP_DISC_REC);
  p_db->mem_free -= sizeof(tSDP_DISC_REC);

  p_rec->p_first_attr = NULL;
  p_rec->p_next_rec = NULL;

  p_rec->remote_bd_addr = bd_addr;

  /* Add the record to the end of chain */
  if (!p_db->p_first_rec)
    p_db->p_first_rec = p_rec;
  else {
    tSDP_DISC_REC* p_rec1 = p_db->p_first_rec;

    while (p_rec1->p_next_rec) p_rec1 = p_rec1->p_next_rec;

    p_rec1->p_next_rec = p_rec;
  }

  return (p_rec);
}

#define SDP_ADDITIONAL_LIST_MASK 0x80
/*******************************************************************************
 *
 * Function         add_attr
 *
 * Description      This function allocates space for an attribute from the DB
 *                  and copies the data into it.
 *
 * Returns          pointer to next byte in data stream
 *
 ******************************************************************************/
static uint8_t* add_attr(uint8_t* p, uint8_t* p_end, tSDP_DISCOVERY_DB* p_db,
                         tSDP_DISC_REC* p_rec, uint16_t attr_id,
                         tSDP_DISC_ATTR* p_parent_attr, uint8_t nest_level) {
  tSDP_DISC_ATTR* p_attr;
  uint32_t attr_len;
  uint32_t total_len;
  uint16_t attr_type;
  uint16_t id;
  uint8_t type;
  uint8_t* p_attr_end;
  uint8_t is_additional_list = nest_level & SDP_ADDITIONAL_LIST_MASK;

  nest_level &= ~(SDP_ADDITIONAL_LIST_MASK);

  if (p + sizeof(uint8_t) > p_end) {
    log::warn("bad arguments to add_addr");
    return NULL;
  }

  type = *p++;
  p = sdpu_get_len_from_type(p, p_end, type, &attr_len);
  if (p == NULL || (p + attr_len) > p_end) {
    log::warn("bad length in attr_rsp");
    return NULL;
  }
  attr_len &= SDP_DISC_ATTR_LEN_MASK;
  attr_type = (type >> 3) & 0x0f;

  /* See if there is enough space in the database */
  if (attr_len > 4)
    total_len = attr_len - 4 + (uint16_t)sizeof(tSDP_DISC_ATTR);
  else
    total_len = sizeof(tSDP_DISC_ATTR);

  p_attr_end = p + attr_len;
  if (p_attr_end > p_end) {
    log::warn("SDP - Attribute length beyond p_end");
    return NULL;
  }

  /* Ensure it is a multiple of 4 */
  total_len = (total_len + 3) & ~3;

  /* See if there is enough space in the database */
  if (p_db->mem_free < total_len) return (NULL);

  p_attr = (tSDP_DISC_ATTR*)p_db->p_free_mem;
  p_attr->attr_id = attr_id;
  p_attr->attr_len_type = (uint16_t)attr_len | (attr_type << 12);
  p_attr->p_next_attr = NULL;

  /* Store the attribute value */
  switch (attr_type) {
    case UINT_DESC_TYPE:
      if ((is_additional_list != 0) && (attr_len == 2)) {
        BE_STREAM_TO_UINT16(id, p);
        if (id != ATTR_ID_PROTOCOL_DESC_LIST)
          p -= 2;
        else {
          /* Reserve the memory for the attribute now, as we need to add
           * sub-attributes */
          p_db->p_free_mem += sizeof(tSDP_DISC_ATTR);
          p_db->mem_free -= sizeof(tSDP_DISC_ATTR);
          total_len = 0;

          /* LOG_VERBOSE ("SDP - attr nest level:%d(list)", nest_level); */
          if (nest_level >= MAX_NEST_LEVELS) {
            log::error("SDP - attr nesting too deep");
            return p_attr_end;
          }

          /* Now, add the list entry */
          p = add_attr(p, p_end, p_db, p_rec, ATTR_ID_PROTOCOL_DESC_LIST,
                       p_attr, (uint8_t)(nest_level + 1));

          break;
        }
      }
      FALLTHROUGH_INTENDED; /* FALLTHROUGH */

    case TWO_COMP_INT_DESC_TYPE:
      switch (attr_len) {
        case 1:
          p_attr->attr_value.v.u8 = *p++;
          break;
        case 2:
          BE_STREAM_TO_UINT16(p_attr->attr_value.v.u16, p);
          break;
        case 4:
          BE_STREAM_TO_UINT32(p_attr->attr_value.v.u32, p);
          break;
        default:
          BE_STREAM_TO_ARRAY(p, p_attr->attr_value.v.array, (int32_t)attr_len);
          break;
      }
      break;

    case UUID_DESC_TYPE:
      switch (attr_len) {
        case 2:
          BE_STREAM_TO_UINT16(p_attr->attr_value.v.u16, p);
          break;
        case 4:
          BE_STREAM_TO_UINT32(p_attr->attr_value.v.u32, p);
          if (p_attr->attr_value.v.u32 < 0x10000) {
            attr_len = 2;
            p_attr->attr_len_type = (uint16_t)attr_len | (attr_type << 12);
            p_attr->attr_value.v.u16 = (uint16_t)p_attr->attr_value.v.u32;
          }
          break;
        case 16:
          /* See if we can compress the UUID down to 16 or 32bit UUIDs */
          if (sdpu_is_base_uuid(p)) {
            if ((p[0] == 0) && (p[1] == 0)) {
              p_attr->attr_len_type =
                  (p_attr->attr_len_type & ~SDP_DISC_ATTR_LEN_MASK) | 2;
              p += 2;
              BE_STREAM_TO_UINT16(p_attr->attr_value.v.u16, p);
              p += Uuid::kNumBytes128 - 4;
            } else {
              p_attr->attr_len_type =
                  (p_attr->attr_len_type & ~SDP_DISC_ATTR_LEN_MASK) | 4;
              BE_STREAM_TO_UINT32(p_attr->attr_value.v.u32, p);
              p += Uuid::kNumBytes128 - 4;
            }
          } else {
            BE_STREAM_TO_ARRAY(p, p_attr->attr_value.v.array,
                               (int32_t)attr_len);
          }
          break;
        default:
          log::warn("SDP - bad len in UUID attr: {}", attr_len);
          return p_attr_end;
      }
      break;

    case DATA_ELE_SEQ_DESC_TYPE:
    case DATA_ELE_ALT_DESC_TYPE:
      /* Reserve the memory for the attribute now, as we need to add
       * sub-attributes */
      p_db->p_free_mem += sizeof(tSDP_DISC_ATTR);
      p_db->mem_free -= sizeof(tSDP_DISC_ATTR);
      total_len = 0;

      /* LOG_VERBOSE ("SDP - attr nest level:%d", nest_level); */
      if (nest_level >= MAX_NEST_LEVELS) {
        log::error("SDP - attr nesting too deep");
        return p_attr_end;
      }
      if (is_additional_list != 0 ||
          attr_id == ATTR_ID_ADDITION_PROTO_DESC_LISTS)
        nest_level |= SDP_ADDITIONAL_LIST_MASK;
      /* LOG_VERBOSE ("SDP - attr nest level:0x%x(finish)", nest_level); */

      while (p < p_attr_end) {
        /* Now, add the list entry */
        p = add_attr(p, p_end, p_db, p_rec, 0, p_attr,
                     (uint8_t)(nest_level + 1));

        if (!p) return (NULL);
      }
      break;

    case TEXT_STR_DESC_TYPE:
    case URL_DESC_TYPE:
      BE_STREAM_TO_ARRAY(p, p_attr->attr_value.v.array, (int32_t)attr_len);
      break;

    case BOOLEAN_DESC_TYPE:
      switch (attr_len) {
        case 1:
          p_attr->attr_value.v.u8 = *p++;
          break;
        default:
          log::warn("SDP - bad len in boolean attr: {}", attr_len);
          return p_attr_end;
      }
      break;

    default: /* switch (attr_type) */
      break;
  }

  p_db->p_free_mem += total_len;
  p_db->mem_free -= total_len;

  /* Add the attribute to the end of the chain */
  if (!p_parent_attr) {
    if (!p_rec->p_first_attr)
      p_rec->p_first_attr = p_attr;
    else {
      tSDP_DISC_ATTR* p_attr1 = p_rec->p_first_attr;

      while (p_attr1->p_next_attr) p_attr1 = p_attr1->p_next_attr;

      p_attr1->p_next_attr = p_attr;
    }
  } else {
    if (!p_parent_attr->attr_value.v.p_sub_attr) {
      p_parent_attr->attr_value.v.p_sub_attr = p_attr;
      /* LOG_VERBOSE ("parent:0x%x(id:%d), ch:0x%x(id:%d)",
          p_parent_attr, p_parent_attr->attr_id, p_attr, p_attr->attr_id); */
    } else {
      tSDP_DISC_ATTR* p_attr1 = p_parent_attr->attr_value.v.p_sub_attr;
      /* LOG_VERBOSE ("parent:0x%x(id:%d), ch1:0x%x(id:%d)",
          p_parent_attr, p_parent_attr->attr_id, p_attr1, p_attr1->attr_id); */

      while (p_attr1->p_next_attr) p_attr1 = p_attr1->p_next_attr;

      p_attr1->p_next_attr = p_attr;
      /* LOG_VERBOSE ("new ch:0x%x(id:%d)", p_attr, p_attr->attr_id); */
    }
  }

  return (p);
}

/*******************************************************************************
 *
 * Function         save_attr_seq
 *
 * Description      This function is called when there is a response from
 *                  the server.
 *
 * Returns          pointer to next byte or NULL if error
 *
 ******************************************************************************/
static uint8_t* save_attr_seq(tCONN_CB* p_ccb, uint8_t* p, uint8_t* p_msg_end) {
  uint32_t seq_len, attr_len;
  uint16_t attr_id;
  uint8_t type, *p_seq_end;
  tSDP_DISC_REC* p_rec;

  type = *p++;

  if ((type >> 3) != DATA_ELE_SEQ_DESC_TYPE) {
    log::warn("SDP - Wrong type: 0x{:02x} in attr_rsp", type);
    return (NULL);
  }
  p = sdpu_get_len_from_type(p, p_msg_end, type, &seq_len);
  if (p == NULL || (p + seq_len) > p_msg_end) {
    log::warn("SDP - Bad len in attr_rsp {}", seq_len);
    return (NULL);
  }

  /* Create a record */
  p_rec = add_record(p_ccb->p_db, p_ccb->device_address);
  if (!p_rec) {
    log::warn("SDP - DB full add_record");
    return (NULL);
  }

  p_seq_end = p + seq_len;

  while (p < p_seq_end) {
    /* First get the attribute ID */
    type = *p++;
    p = sdpu_get_len_from_type(p, p_msg_end, type, &attr_len);
    if (p == NULL || (p + attr_len) > p_seq_end) {
      log::warn("Bad len in attr_rsp {}", attr_len);
      return (NULL);
    }
    if (((type >> 3) != UINT_DESC_TYPE) || (attr_len != 2)) {
      log::warn("SDP - Bad type: 0x{:02x} or len: {} in attr_rsp", type,
                attr_len);
      return (NULL);
    }
    BE_STREAM_TO_UINT16(attr_id, p);

    /* Now, add the attribute value */
    p = add_attr(p, p_seq_end, p_ccb->p_db, p_rec, attr_id, NULL, 0);

    if (!p) {
      log::warn("SDP - DB full add_attr");
      return (NULL);
    }
  }

  return (p);
}

/*******************************************************************************
 *
 * Function         process_service_search_attr_rsp
 *
 * Description      This function is called when there is a search attribute
 *                  response from the server.
 *
 * Returns          void
 *
 ******************************************************************************/
static void process_service_search_attr_rsp(tCONN_CB* p_ccb, uint8_t* p_reply,
                                            uint8_t* p_reply_end) {
  uint8_t *p, *p_start, *p_end, *p_param_len;
  uint8_t type;
  uint32_t seq_len;
  uint16_t param_len, lists_byte_count = 0;
  bool cont_request_needed = false;

  /* If p_reply is NULL, we were called for the initial read */
  if (p_reply) {
    if (p_reply + 4 /* transaction ID and length */ + sizeof(lists_byte_count) >
        p_reply_end) {
      sdp_disconnect(p_ccb, SDP_INVALID_PDU_SIZE);
      return;
    }

    /* Skip transaction ID and length */
    p_reply += 4;

    BE_STREAM_TO_UINT16(lists_byte_count, p_reply);

    /* Copy the response to the scratchpad. First, a safety check on the length
     */
    if ((p_ccb->list_len + lists_byte_count) > SDP_MAX_LIST_BYTE_COUNT) {
      sdp_disconnect(p_ccb, SDP_INVALID_PDU_SIZE);
      return;
    }

    if (p_reply + lists_byte_count + 1 /* continuation */ > p_reply_end) {
      sdp_disconnect(p_ccb, SDP_INVALID_PDU_SIZE);
      return;
    }

    if (p_ccb->rsp_list == NULL)
      p_ccb->rsp_list = (uint8_t*)osi_malloc(SDP_MAX_LIST_BYTE_COUNT);
    memcpy(&p_ccb->rsp_list[p_ccb->list_len], p_reply, lists_byte_count);
    p_ccb->list_len += lists_byte_count;
    p_reply += lists_byte_count;
    if (*p_reply) {
      if (*p_reply > SDP_MAX_CONTINUATION_LEN) {
        sdp_disconnect(p_ccb, SDP_INVALID_CONT_STATE);
        return;
      }

      cont_request_needed = true;
    }
  }

  /* If continuation request (or first time request) */
  if ((cont_request_needed) || (!p_reply)) {
    BT_HDR* p_msg = (BT_HDR*)osi_malloc(SDP_DATA_BUF_SIZE);
    uint8_t* p;
    uint16_t bytes_left = SDP_DATA_BUF_SIZE;

    /* If we don't have a valid discovery database, we can't do anything. */
    if (p_ccb->p_db == NULL) {
      log::warn(
          "Attempted continuation or first time request with invalid discovery "
          "database");
      sdp_disconnect(p_ccb, tSDP_STATUS::SDP_INVALID_CONT_STATE);
      osi_free(p_msg);
      return;
    }

    p_msg->offset = L2CAP_MIN_OFFSET;
    p = p_start = (uint8_t*)(p_msg + 1) + L2CAP_MIN_OFFSET;

    /* Build a service search request packet */
    UINT8_TO_BE_STREAM(p, SDP_PDU_SERVICE_SEARCH_ATTR_REQ);
    UINT16_TO_BE_STREAM(p, p_ccb->transaction_id);
    p_ccb->transaction_id++;

    /* Skip the length, we need to add it at the end */
    p_param_len = p;
    p += 2;

    /* Account for header size, max service record count and
     * continuation state */
    const uint16_t base_bytes = (sizeof(BT_HDR) + L2CAP_MIN_OFFSET +
                                 3u + /* service search request header */
                                 2u + /* param len */
                                 3u + /* max service record count */
                                 ((p_reply) ? (*p_reply) : 0));

    if (base_bytes > bytes_left) {
      sdp_disconnect(p_ccb, SDP_INVALID_CONT_STATE);
      osi_free(p_msg);
      return;
    }

    bytes_left -= base_bytes;

    /* Build the UID sequence. */
    p = sdpu_build_uuid_seq(p, p_ccb->p_db->num_uuid_filters,
                            p_ccb->p_db->uuid_filters, bytes_left);

    /* Max attribute byte count */
    UINT16_TO_BE_STREAM(p, sdp_cb.max_attr_list_size);

    /* If no attribute filters, build a wildcard attribute sequence */
    if (p_ccb->p_db->num_attr_filters)
      p = sdpu_build_attrib_seq(p, p_ccb->p_db->attr_filters,
                                p_ccb->p_db->num_attr_filters);
    else
      p = sdpu_build_attrib_seq(p, NULL, 0);

    /* No continuation for first request */
    if (p_reply) {
      if ((p_reply + *p_reply + 1) <= p_reply_end) {
        memcpy(p, p_reply, *p_reply + 1);
        p += *p_reply + 1;
      }
    } else
      UINT8_TO_BE_STREAM(p, 0);

    /* Go back and put the parameter length into the buffer */
    param_len = p - p_param_len - 2;
    UINT16_TO_BE_STREAM(p_param_len, param_len);

    /* Set the length of the SDP data in the buffer */
    p_msg->len = p - p_start;

    if (L2CA_DataWrite(p_ccb->connection_id, p_msg) != L2CAP_DW_SUCCESS) {
      log::warn("Unable to write L2CAP data peer:{} cid:{} len:{}",
                p_ccb->device_address, p_ccb->connection_id, p - p_start);
    }

    /* Start inactivity timer */
    alarm_set_on_mloop(p_ccb->sdp_conn_timer, SDP_INACT_TIMEOUT_MS,
                       sdp_conn_timer_timeout, p_ccb);

    return;
  }

/*******************************************************************/
/* We now have the full response, which is a sequence of sequences */
/*******************************************************************/

  if (!sdp_copy_raw_data(p_ccb, true)) {
    log::error("sdp_copy_raw_data failed");
    sdp_disconnect(p_ccb, SDP_ILLEGAL_PARAMETER);
    return;
  }

  p = &p_ccb->rsp_list[0];

  /* The contents is a sequence of attribute sequences */
  type = *p++;

  if ((type >> 3) != DATA_ELE_SEQ_DESC_TYPE) {
    log::warn("Wrong element in attr_rsp type:0x{:02x}", type);
    sdp_disconnect(p_ccb, SDP_ILLEGAL_PARAMETER);
    return;
  }
  p = sdpu_get_len_from_type(p, p + p_ccb->list_len, type, &seq_len);
  if (p == NULL || (p + seq_len) > (p + p_ccb->list_len)) {
    log::warn("Illegal search attribute length");
    sdp_disconnect(p_ccb, SDP_ILLEGAL_PARAMETER);
    return;
  }
  p_end = &p_ccb->rsp_list[p_ccb->list_len];

  if ((p + seq_len) != p_end) {
    sdp_disconnect(p_ccb, SDP_INVALID_CONT_STATE);
    return;
  }

  while (p < p_end) {
    p = save_attr_seq(p_ccb, p, &p_ccb->rsp_list[p_ccb->list_len]);
    if (!p) {
      sdp_disconnect(p_ccb, SDP_DB_FULL);
      return;
    }
  }

  /* Since we got everything we need, disconnect the call */
  sdpu_log_attribute_metrics(p_ccb->device_address, p_ccb->p_db);
  sdp_disconnect(p_ccb, SDP_SUCCESS);
}

/*******************************************************************************
 *
 * Function         process_service_attr_rsp
 *
 * Description      This function is called when there is a attribute response
 *                  from the server.
 *
 * Returns          void
 *
 ******************************************************************************/
static void process_service_attr_rsp(tCONN_CB* p_ccb, uint8_t* p_reply,
                                     uint8_t* p_reply_end) {
  uint8_t *p_start, *p_param_len;
  uint16_t param_len, list_byte_count;
  bool cont_request_needed = false;

  /* If p_reply is NULL, we were called after the records handles were read */
  if (p_reply) {
    if (p_reply + 4 /* transaction ID and length */ + sizeof(list_byte_count) >
        p_reply_end) {
      sdp_disconnect(p_ccb, SDP_INVALID_PDU_SIZE);
      return;
    }

    /* Skip transaction ID and length */
    p_reply += 4;

    BE_STREAM_TO_UINT16(list_byte_count, p_reply);

    /* Copy the response to the scratchpad. First, a safety check on the length
     */
    if ((p_ccb->list_len + list_byte_count) > SDP_MAX_LIST_BYTE_COUNT) {
      sdp_disconnect(p_ccb, SDP_INVALID_PDU_SIZE);
      return;
    }

    if (p_reply + list_byte_count + 1 /* continuation */ > p_reply_end) {
      sdp_disconnect(p_ccb, SDP_INVALID_PDU_SIZE);
      return;
    }

    if (p_ccb->rsp_list == NULL)
      p_ccb->rsp_list = (uint8_t*)osi_malloc(SDP_MAX_LIST_BYTE_COUNT);
    memcpy(&p_ccb->rsp_list[p_ccb->list_len], p_reply, list_byte_count);
    p_ccb->list_len += list_byte_count;
    p_reply += list_byte_count;
    if (*p_reply) {
      if (*p_reply > SDP_MAX_CONTINUATION_LEN) {
        sdp_disconnect(p_ccb, SDP_INVALID_CONT_STATE);
        return;
      }
      cont_request_needed = true;
    } else {
      log::warn("process_service_attr_rsp");
      if (!sdp_copy_raw_data(p_ccb, false)) {
        log::error("sdp_copy_raw_data failed");
        sdp_disconnect(p_ccb, SDP_ILLEGAL_PARAMETER);
        return;
      }

      /* Save the response in the database. Stop on any error */
      if (!save_attr_seq(p_ccb, &p_ccb->rsp_list[0],
                         &p_ccb->rsp_list[p_ccb->list_len])) {
        sdp_disconnect(p_ccb, SDP_DB_FULL);
        return;
      }
      p_ccb->list_len = 0;
      p_ccb->cur_handle++;
    }
  }

  /* Now, ask for the next handle. Re-use the buffer we just got. */
  if (p_ccb->cur_handle < p_ccb->num_handles) {
    BT_HDR* p_msg = (BT_HDR*)osi_malloc(SDP_DATA_BUF_SIZE);
    uint8_t* p;

    p_msg->offset = L2CAP_MIN_OFFSET;
    p = p_start = (uint8_t*)(p_msg + 1) + L2CAP_MIN_OFFSET;

    /* Get all the attributes from the server */
    UINT8_TO_BE_STREAM(p, SDP_PDU_SERVICE_ATTR_REQ);
    UINT16_TO_BE_STREAM(p, p_ccb->transaction_id);
    p_ccb->transaction_id++;

    /* Skip the length, we need to add it at the end */
    p_param_len = p;
    p += 2;

    UINT32_TO_BE_STREAM(p, p_ccb->handles[p_ccb->cur_handle]);

    /* Max attribute byte count */
    UINT16_TO_BE_STREAM(p, sdp_cb.max_attr_list_size);

    /* If no attribute filters, build a wildcard attribute sequence */
    if (p_ccb->p_db->num_attr_filters)
      p = sdpu_build_attrib_seq(p, p_ccb->p_db->attr_filters,
                                p_ccb->p_db->num_attr_filters);
    else
      p = sdpu_build_attrib_seq(p, NULL, 0);

    /* Was this a continuation request ? */
    if (cont_request_needed) {
      if ((p_reply + *p_reply + 1) <= p_reply_end) {
        memcpy(p, p_reply, *p_reply + 1);
        p += *p_reply + 1;
      }
    } else
      UINT8_TO_BE_STREAM(p, 0);

    /* Go back and put the parameter length into the buffer */
    param_len = (uint16_t)(p - p_param_len - 2);
    UINT16_TO_BE_STREAM(p_param_len, param_len);

    /* Set the length of the SDP data in the buffer */
    p_msg->len = (uint16_t)(p - p_start);

    if (L2CA_DataWrite(p_ccb->connection_id, p_msg) != L2CAP_DW_SUCCESS) {
      log::warn("Unable to write L2CAP data peer:{} cid:{} len:{}",
                p_ccb->device_address, p_ccb->connection_id, p - p_start);
    }

    /* Start inactivity timer */
    alarm_set_on_mloop(p_ccb->sdp_conn_timer, SDP_INACT_TIMEOUT_MS,
                       sdp_conn_timer_timeout, p_ccb);
  } else {
    sdpu_log_attribute_metrics(p_ccb->device_address, p_ccb->p_db);
    sdp_disconnect(p_ccb, SDP_SUCCESS);
    return;
  }
}

/******************************************************************************
 *
 * Function         process_service_search_rsp
 *
 * Description      This function is called when there is a search response from
 *                  the server.
 *
 * Returns          void
 *
 ******************************************************************************/
static void process_service_search_rsp(tCONN_CB* p_ccb, uint8_t* p_reply,
                                       uint8_t* p_reply_end) {
  uint16_t xx;
  uint16_t total, cur_handles, orig;
  uint8_t cont_len;

  if (p_reply + 8 > p_reply_end) {
    sdp_disconnect(p_ccb, SDP_GENERIC_ERROR);
    return;
  }
  /* Skip transaction, and param len */
  p_reply += 4;
  BE_STREAM_TO_UINT16(total, p_reply);
  BE_STREAM_TO_UINT16(cur_handles, p_reply);

  orig = p_ccb->num_handles;
  p_ccb->num_handles += cur_handles;
  if (p_ccb->num_handles == 0 || p_ccb->num_handles < orig) {
    log::warn("SDP - Rcvd ServiceSearchRsp, no matches");
    sdp_disconnect(p_ccb, SDP_NO_RECS_MATCH);
    return;
  }

  /* Save the handles that match. We will can only process a certain number. */
  if (total > sdp_cb.max_recs_per_search) total = sdp_cb.max_recs_per_search;
  if (p_ccb->num_handles > sdp_cb.max_recs_per_search)
    p_ccb->num_handles = sdp_cb.max_recs_per_search;

  if (p_reply + ((p_ccb->num_handles - orig) * 4) + 1 > p_reply_end) {
    sdp_disconnect(p_ccb, SDP_GENERIC_ERROR);
    return;
  }

  for (xx = orig; xx < p_ccb->num_handles; xx++)
    BE_STREAM_TO_UINT32(p_ccb->handles[xx], p_reply);

  BE_STREAM_TO_UINT8(cont_len, p_reply);
  if (cont_len != 0) {
    if (cont_len > SDP_MAX_CONTINUATION_LEN) {
      sdp_disconnect(p_ccb, SDP_INVALID_CONT_STATE);
      return;
    }
    if (p_reply + cont_len > p_reply_end) {
      sdp_disconnect(p_ccb, SDP_INVALID_CONT_STATE);
      return;
    }
    /* stay in the same state */
    sdp_snd_service_search_req(p_ccb, cont_len, p_reply);
  } else {
    /* change state */
    p_ccb->disc_state = SDP_DISC_WAIT_ATTR;

    /* Kick off the first attribute request */
    process_service_attr_rsp(p_ccb, NULL, NULL);
  }
}

/*******************************************************************************
 *
 * Function         sdp_disc_connected
 *
 * Description      This function is called when an SDP discovery attempt is
 *                  connected.
 *
 * Returns          void
 *
 ******************************************************************************/
void sdp_disc_connected(tCONN_CB* p_ccb) {
  if (p_ccb->is_attr_search) {
    p_ccb->disc_state = SDP_DISC_WAIT_SEARCH_ATTR;

    process_service_search_attr_rsp(p_ccb, NULL, NULL);
  } else {
    /* First step is to get a list of the handles from the server. */
    /* We are not searching for a specific attribute, so we will   */
    /* first search for the service, then get all attributes of it */

    p_ccb->num_handles = 0;
    sdp_snd_service_search_req(p_ccb, 0, NULL);
  }
}

/*******************************************************************************
 *
 * Function         sdp_disc_server_rsp
 *
 * Description      This function is called when there is a response from
 *                  the server.
 *
 * Returns          void
 *
 ******************************************************************************/
void sdp_disc_server_rsp(tCONN_CB* p_ccb, BT_HDR* p_msg) {
  uint8_t *p, rsp_pdu;
  bool invalid_pdu = true;

  /* stop inactivity timer when we receive a response */
  alarm_cancel(p_ccb->sdp_conn_timer);

  /* Got a reply!! Check what we got back */
  p = (uint8_t*)(p_msg + 1) + p_msg->offset;
  uint8_t* p_end = p + p_msg->len;

  if (p_msg->len < 1) {
    sdp_disconnect(p_ccb, SDP_GENERIC_ERROR);
    return;
  }

  BE_STREAM_TO_UINT8(rsp_pdu, p);

  p_msg->len--;

  switch (rsp_pdu) {
    case SDP_PDU_SERVICE_SEARCH_RSP:
      if (p_ccb->disc_state == SDP_DISC_WAIT_HANDLES) {
        process_service_search_rsp(p_ccb, p, p_end);
        invalid_pdu = false;
      }
      break;

    case SDP_PDU_SERVICE_ATTR_RSP:
      if (p_ccb->disc_state == SDP_DISC_WAIT_ATTR) {
        process_service_attr_rsp(p_ccb, p, p_end);
        invalid_pdu = false;
      }
      break;

    case SDP_PDU_SERVICE_SEARCH_ATTR_RSP:
      if (p_ccb->disc_state == SDP_DISC_WAIT_SEARCH_ATTR) {
        process_service_search_attr_rsp(p_ccb, p, p_end);
        invalid_pdu = false;
      }
      break;
  }

  if (invalid_pdu) {
    log::warn("SDP - Unexp. PDU: {} in state: {}", rsp_pdu, p_ccb->disc_state);
    sdp_disconnect(p_ccb, SDP_GENERIC_ERROR);
  }
}
