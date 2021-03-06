
#include <net/if.h>
#include <ifaddrs.h>
#include <sys/types.h>

#include "module.h"
#include "mq.h"
#include "ice_channel.h"
#include "ice_session.h"
#include "json/json.h"
#include "sdp.h"
#include "rtcp.h"
#include "process.h"
#include "utils.h"

/* FIXME: standardize sdp */
static int
snw_ice_generate_base_sdp(snw_ice_context_t *ice_ctx, 
      char *sdp, uint32_t len, int sendonly) {
   static const char *sdp_template = 
         "v=0\r\no=- %lu %lu IN IP4 127.0.0.1\r\ns=%s\r\nt=0 0\r\n%s%s";
   static  const char *audio_mline_template = 
         "m=audio 1 RTP/SAVPF %d\r\nc=IN IP4 1.1.1.1\r\na=%s\r\na=rtpmap:%d opus/48000/2\r\n";
   static  const char *video_mline_template = 
         "m=video 1 RTP/SAVPF %d\r\nc=IN IP4 1.1.1.1\r\na=%s\r\na=rtpmap:%d VP8/90000\r\n"
         "a=rtcp-fb:%d ccm fir\r\na=rtcp-fb:%d nack\r\na=rtcp-fb:%d nack pli\r\na=rtcp-fb:%d goog-remb\r\n";
   static char audio_mline[256], video_mline[512];
   snw_log_t *log = 0;

   if (!ice_ctx) return -1;
   log = ice_ctx->log;

   DEBUG(log,"sendonly=%u",sendonly);

   memset(audio_mline,0,512);
   snprintf(audio_mline, 256, audio_mline_template,
       OPUS_PT, sendonly ? "sendonly" : "sendrecv", OPUS_PT);

   memset(video_mline,0,512);
   snprintf(video_mline, 512, video_mline_template,
       VP8_PT, sendonly ? "sendonly" : "sendrecv",
       VP8_PT, VP8_PT, VP8_PT, VP8_PT, VP8_PT);

   memset(sdp,0,len);
   snprintf(sdp, len, sdp_template,
       get_real_time(), get_real_time(),
       "PeerCall Replay", audio_mline, video_mline);

   return 0;
}


void ice_send_candidate(snw_ice_session_t *session, int video, char *buffer, int len) {
   snw_context_t *ctx;
   snw_log_t *log = 0;
   Json::Value root,candidate;
   std::string output;
   Json::FastWriter writer;
   std::string str(buffer,len);

   if (!session) return;
   ctx = (snw_context_t*)session->ice_ctx->ctx;
   log = session->ice_ctx->log;

   root["msgtype"] = SNW_ICE;
   root["api"] = SNW_ICE_CANDIDATE;
   root["roomid"] = 0;
   root["callid"] = "callid";
   candidate["type"] = "candidate";
   if (video) {
      candidate["label"] = 1;
      candidate["id"] = "video";
   } else {
      candidate["label"] = 0;
      candidate["id"] = "audio";
   }
   candidate["candidate"] = str;
   root["candidate"] = candidate;

   output = writer.write(root);
   DEBUG(log, "Sending sdp, sdp=%s",output.c_str());
   snw_shmmq_enqueue(ctx->snw_ice2core_mq,0,output.c_str(),output.size(),session->flowid);

   return;
}

void
snw_ice_send_local_candidate(snw_ice_session_t *session, int video, uint32_t stream_id, uint32_t component_id) {
   snw_log_t *log = 0;
   agent_t* agent = 0;
   snw_ice_stream_t *stream = 0;
   snw_ice_component_t *component = 0;
   struct list_head *i,*n;
   candidate_t *candidates;
   int len;

   if (!session || !session->agent)
      return;
   log = session->ice_ctx->log;

   agent = session->agent;
   stream = snw_stream_find(&session->streams, stream_id);
   if(!stream) {
      ERROR(log, "No stream %d", stream_id);
      return;
   }

   component = snw_component_find(&stream->components, component_id);
   if (!component) {
      ERROR(log, "No component %d in stream %d", component_id, stream_id);
      return;
   }

   candidates = ice_agent_get_local_candidates(agent, stream_id, component_id);
   if (candidates == 0 )
      return;

   DEBUG(log, "got candidates, size=%u, sid=%u, cid=%u",
         list_size(&candidates->list), stream_id, component_id);

   list_for_each_safe(i,n,&candidates->list) {
      char buffer[100] = {0};
      candidate_t *c = list_entry(i,candidate_t,list);
      char address[ICE_ADDRESS_STRING_LEN], base_address[ICE_ADDRESS_STRING_LEN];
      int port = 0, base_port = 0;
      address_to_string(&(c->addr), (char *)&address);
      port = address_get_port(&(c->addr));
      address_to_string(&(c->base_addr), (char *)&base_address);
      base_port = address_get_port(&(c->base_addr));

      DEBUG(log, "candidate info, sid=%u, cid=%u, addr=%s, port=%u, priority=%u, foundation=%u",
            c->stream_id, c->component_id, address, port, c->priority, c->foundation);

      if (c->type == ICE_CANDIDATE_TYPE_HOST) {
         if (c->transport == ICE_CANDIDATE_TRANSPORT_UDP) {
            len = snprintf(buffer, 100, "candidate:%s %d %s %d %s %d typ host generation 0",
                  c->foundation, c->component_id, "udp", c->priority, address, port);
         } else {
            DEBUG(log, "only ice-udp supported");
            candidate_free(c);
            continue;
         }
      } else if (c->type == ICE_CANDIDATE_TYPE_SERVER_REFLEXIVE) {
         if (c->transport == ICE_CANDIDATE_TRANSPORT_UDP) {
            address_to_string(&(c->base_addr), (char *)&base_address);
            int base_port = address_get_port(&(c->base_addr));
            len = snprintf(buffer, 100, "candidate:%s %d %s %d %s %d typ srflx raddr %s rport %d",
                  c->foundation, c->component_id, "udp", c->priority, address, port, base_address, base_port);
         } else {
            DEBUG(log, "only ice-udp supported");
            candidate_free(c);
            continue;
         }
      } else if(c->type == ICE_CANDIDATE_TYPE_PEER_REFLEXIVE) {
         DEBUG(log, "skipping prflx candidate");
         candidate_free(c);
         continue;
      } else if(c->type == ICE_CANDIDATE_TYPE_RELAYED) {
         if(c->transport == ICE_CANDIDATE_TRANSPORT_UDP) {
            len = snprintf(buffer, 100, "candidate:%s %d %s %d %s %d typ relay raddr %s rport %d",
                  c->foundation, c->component_id, "udp", c->priority, address, port, base_address, base_port);
         } else {
            DEBUG(log, "only ice-udp supported");
            candidate_free(c);
            continue;
         }
      }
      DEBUG(log, "output, len=%d, sdp=%s", len, buffer);
      if ( len > 0 ) {
         ice_send_candidate(session,video,buffer,len);
      }

      candidate_free(c);
   }

   //FXIME: free list of candidates
   /*list_for_each_safe(i,n,&candidates->list) {
      candidate_t *c = list_entry(i,candidate_t,list);
      candidate_free(c);
      list_del(i);
   }*/

   return;
}

void
snw_ice_sdp_send_candidates(snw_ice_session_t *session, int video) {
   snw_ice_stream_t *stream = 0;

   if (video) {
      uint32_t id = session->video_stream->id;
      if (id == 0 && IS_FLAG(session, WEBRTC_BUNDLE))
          id = session->audio_stream->id > 0 ? 
                   session->audio_stream->id : 
                   session->video_stream->id;
      stream = snw_stream_find(&session->streams, id);
   } else {
      stream = snw_stream_find(&session->streams, session->audio_stream->id);
   }

   if (stream == 0)
      return;

   snw_ice_send_local_candidate(session, video, stream->id, 1);
   if (!SET_FLAG(session, WEBRTC_RTCPMUX))
      snw_ice_send_local_candidate(session, video, stream->id, 2);

   return;
}

void
snw_ice_create_msg(snw_ice_context_t *ice_ctx, Json::Value &root, uint32_t flowid) {
   snw_context_t *ctx = (snw_context_t*)ice_ctx->ctx;
   snw_log_t *log = ice_ctx->log;
   snw_ice_channel_t *channel = 0;
   Json::FastWriter writer;
   std::string output;
   int is_new = 0;


   //FIXME: mechanism of channel (de)allocation 
   channel = (snw_ice_channel_t*)snw_ice_channel_get(ice_ctx,flowid,&is_new);

   try {
      if (!channel || !is_new) {
         root["rc"] = -1;
         output = writer.write(root);
         DEBUG(log,"failed to create ice channel, flowid=%u, is_new=%u, len=%u, res=%s", 
               flowid, is_new, output.size(), output.c_str());
         snw_shmmq_enqueue(ctx->snw_ice2core_mq,0,output.c_str(),output.size(),flowid);
         return;
      }
      channel->ownerid = flowid;
      root["id"] = flowid;
      root["channelid"] = flowid; //FIXME: create 'real' sessionid
      root["rc"] = 0;
      output = writer.write(root);

      DEBUG(log,"ice create, mq=%p, flowid=%u, len=%u, res=%s", 
            ctx->snw_ice2core_mq, flowid, output.size(), output.c_str());

      snw_shmmq_enqueue(ctx->snw_ice2core_mq,0,output.c_str(),output.size(),flowid);
   } catch (...) {
      ERROR(log, "json format error, data=%s", output.c_str());
      return;
   }

   return;
}

int
verify_disabled_streams(snw_ice_session_t *session, int audio, int video, const char *jsep_sdp) {
   snw_log_t *log = 0;

   if (!session) return -1;
   log = session->ice_ctx->log;

   if (strstr(jsep_sdp, "m=audio 0")) {
      if(!IS_FLAG(session, WEBRTC_BUNDLE) || !video) {
         snw_ice_stream_t *stream = snw_stream_find(&session->streams, session->audio_stream->id);
         if (stream) {
            DEBUG(log, "disable audio stream, sid=%u",stream->id);
            stream->is_disable = 1;
         }
      }
   }

   if (strstr(jsep_sdp, "m=video 0")) {
      if (!IS_FLAG(session, WEBRTC_BUNDLE) || !audio) {
         snw_ice_stream_t *stream = 0;
         if (!IS_FLAG(session, WEBRTC_BUNDLE)) {
            stream = snw_stream_find(&session->streams, session->video_stream->id);
         } else {
            uint32_t id = session->audio_stream->id > 0 ? 
                              session->audio_stream->id : 
                              session->video_stream->id;
            stream = snw_stream_find(&session->streams, id);
         }
         if (stream) {
            DEBUG(log, "disable video stream, sid=%u",stream->id);
            stream->is_disable = 1;
         }
      }
   }

   return 0;
}


int
snw_ice_generate_sdp(snw_ice_session_t *session) {
   static char base_sdp[1024];
   snw_log_t *log = session->ice_ctx->log;
   ice_sdp_attr_t sdp_attr;
   char *sdp_merged;

   if (!session)
      return -1;

   snw_ice_generate_base_sdp(session->ice_ctx,base_sdp,1024,0);
   DEBUG(log, "sdp info, base_sdp=%s",base_sdp);
   snw_ice_get_sdp_attr(session->ice_ctx,base_sdp,&sdp_attr);

   sdp_merged = snw_ice_sdp_merge(session, base_sdp);
   if (!sdp_merged) {
      return -2;
   }

   verify_disabled_streams(session,sdp_attr.audio, sdp_attr.video, sdp_merged);
   session->local_sdp = sdp_merged;
   //DEBUG("generated sdp, local_sdp=%s",sdp_merged);

   return 0;
}

static void
snw_ice_cb_candidate_gathering_done(agent_t *agent, uint32_t stream_id, void *user_data) {
   snw_ice_session_t *session = (snw_ice_session_t *)user_data;
   snw_context_t *ctx = (snw_context_t*)session->ice_ctx->ctx;
   snw_log_t *log = session->ice_ctx->log;
   Json::Value root,sdp;
   std::string output;
   Json::FastWriter writer;
   int ret = 0;

   if (!session) return;

   session->streams_gathering_done++;
   DEBUG(log, "gathering done, user_data=%p, stream=%d, cdone=%u, streams_num=%u",
          user_data, stream_id, session->streams_gathering_done, session->streams_num);

   snw_ice_stream_t *stream = snw_stream_find(&session->streams, stream_id);
   if (!stream) {
      DEBUG(log, "no stream, stream_id=%d", stream_id);
      return;
   }
   stream->gathering_done = 1;

   if (session->streams_gathering_done == session->streams_num) {
      ret = snw_ice_generate_sdp(session);
      if (ret < 0 || !session->local_sdp) {
         ERROR(log, "failed to generate sdp, ret=%d, local_sdp=%s",ret,session->local_sdp);
      }

      //send sdp into to client.
      DEBUG(log, "Sending local sdp, sdp=%s",session->local_sdp);
      root["msgtype"] = SNW_ICE;
      root["api"] = SNW_ICE_SDP;
      root["sdp"]["type"] = "offer";
      root["sdp"]["sdp"] = session->local_sdp;
      output = writer.write(root);
      DEBUG(log, "Sending ice sdp, flowid=%u, len=%u, sdp=%s", 
                 session->flowid, output.size(), output.c_str());
      snw_shmmq_enqueue(ctx->snw_ice2core_mq,0,output.c_str(),output.size(),session->flowid);

      snw_ice_sdp_send_candidates(session,0);//candidate for audio component
      snw_ice_sdp_send_candidates(session,1);//candidate for video component
   }

   return;
}

void
snw_ice_cb_new_selected_pair(agent_t *agent, uint32_t stream_id,
       uint32_t component_id, char *local, char *remote, void *data) {
   snw_log_t *log = 0;
   snw_ice_session_t *session = 0;
   snw_ice_stream_t *stream = 0;
   snw_ice_component_t *component = 0;

   session = (snw_ice_session_t *)data;
   if (!session) return;
   log = session->ice_ctx->log;

   if (component_id > 1 && IS_FLAG(session, WEBRTC_RTCPMUX)) {
      ERROR(log, "wait for webrtc rtcpmux, component_id=%u",component_id);
      return;
   }

   DEBUG(log, "new selected pair, cid=%d, sid=%d, local=%s, remote=%s",
                component_id, stream_id, local, remote);
   stream = snw_stream_find(&session->streams, stream_id);
   if (!stream) {
      ERROR(log, "stream not found, sid=%d", stream_id);
      return;
   }

   component = snw_component_find(&stream->components, component_id);
   if (!component) {
      ERROR(log, "component not found, cid=%d, sid=%d", component_id, stream_id);
      return;
   }

   if (component->dtls != 0) {
      return;
   }

   component->fir_latest = get_monotonic_time();
   component->dtls = dtls_create(session->ice_ctx, component, stream->dtls_type);
   if (!component->dtls) {
      ERROR(log, "no dtls context");
      return;
   }

   dtls_do_handshake(component->dtls);

   //FIXME: set timeout to call dtls_retry
   return;
}

void
snw_ice_cb_component_state_changed(agent_t *agent,
         uint32_t stream_id, uint32_t component_id, uint32_t state, void *data) {
   snw_ice_session_t *session = (snw_ice_session_t *)data;
   snw_log_t *log = 0;
   snw_ice_stream_t *stream = 0;
   snw_ice_component_t *component = 0;

   if (!session) return;
   log = session->ice_ctx->log;

   DEBUG(log, "component state changed, cid=%u, sid=%u, state=%d",
         component_id, stream_id, state);

   stream = snw_stream_find(&session->streams, stream_id);
   if (!stream) {
      ERROR(log, "No stream %d", stream_id);
      return;
   }

   component = snw_component_find(&stream->components, component_id);
   if (!component) {
      ERROR(log, "No component %d in stream %d", component_id, stream_id);
      return;
   }
   component->state = state;
   if ((state == ICE_COMPONENT_STATE_CONNECTED || state == ICE_COMPONENT_STATE_READY)) {
      //session->ready = 1;
      SET_FLAG(session,WEBRTC_READY);
   }

   if(state == ICE_COMPONENT_STATE_FAILED) {
      ERROR(log, "ice component failed, cid=%u, sid=%u",component_id,stream_id);
   }  

   return;
}  

void
snw_ice_cb_new_remote_candidate(agent_t *agent, uint32_t stream_id,
                     uint32_t component_id, char *foundation, void *data) {
   char address[ICE_ADDRESS_STRING_LEN], base_address[ICE_ADDRESS_STRING_LEN];
   snw_ice_session_t *session = (snw_ice_session_t *)data;
   snw_log_t *log = 0;
   snw_ice_stream_t *stream = 0;
   snw_ice_component_t *component = 0;
   candidate_t *candidate = 0;
   candidate_t *candidates = 0;
   struct list_head *tmp, *n;
   int port = 0, base_port = 0;
   char buffer[100];

   if (!session) return;
   log = session->ice_ctx->log;

   DEBUG(log, "discovered new remote candidate, cid=%d, sid=%d, foundation=%s",
          component_id, stream_id, foundation);

   if (component_id > 1 && IS_FLAG(session, WEBRTC_RTCPMUX)) {
      DEBUG(log, "ignore new candidate, component=%d,rtcpmux=%u",
            component_id, IS_FLAG(session, WEBRTC_RTCPMUX));
      return;
   }

   stream = snw_stream_find(&session->streams, stream_id);
   if (!stream) {
      ERROR(log, "stream not found, sid=%u", stream_id);
      return;
   }

   component = snw_component_find(&stream->components, component_id);
   if (!component) {
      ERROR(log, "component not found, cid=%u, sid=%u", component_id, stream_id);
      return;
   }
   candidates = ice_agent_get_remote_candidates(agent, component_id, stream_id);
   list_for_each_safe(tmp,n,&candidates->list) {
      candidate_t *c = list_entry(tmp,candidate_t,list);
      if(candidate == 0) {
         if(!strcasecmp(c->foundation, foundation)) {
            candidate = c;
            continue;
         }
      }
   }

   if(candidate == 0) {
      DEBUG(log, "candidate not found, foundation %s", foundation);
      return;
   }

   if(candidate->type != ICE_CANDIDATE_TYPE_PEER_REFLEXIVE) {
      DEBUG(log, "candidatedone");
      goto candidatedone;
   }

   DEBUG(log, "stream info, sid=%u, cid=%u", candidate->stream_id, candidate->component_id);

   {//DEBUG
      address_to_string(&(candidate->addr), (char *)&address);
      port = address_get_port(&(candidate->addr));
      address_to_string(&(candidate->base_addr), (char *)&base_address);
      base_port = address_get_port(&(candidate->base_addr));
      DEBUG(log, "Address:    %s:%d", address, port);
      DEBUG(log, "Priority:   %d", candidate->priority);
      DEBUG(log, "Foundation: %s", candidate->foundation);
   }

   if(candidate->transport == ICE_CANDIDATE_TRANSPORT_UDP) {
      snprintf(buffer, 100,
         "%s %d %s %d %s %d typ prflx raddr %s rport %d\r\n",
            candidate->foundation,
            candidate->component_id,
            "udp",
            candidate->priority,
            address,
            port,
            base_address,
            base_port);
   } else {
      ERROR(log, "transport not supported");
   }

candidatedone:
   candidate_free(candidate);
   return;
}

int
snw_ice_add_local_addresses(snw_ice_session_t *session) {
   struct ifaddrs *ifaddr, *ifa;
   int family, s;
   char host[NI_MAXHOST];

   if (getifaddrs(&ifaddr) == -1) {
      //ERROR("Error getting list of interfaces");
      return -1;
   } else {
      for(ifa = ifaddr; ifa != 0; ifa = ifa->ifa_next) {
         address_t addr_local;

         if (ifa->ifa_addr == 0)
            continue;

         if (!((ifa->ifa_flags & IFF_UP) && (ifa->ifa_flags & IFF_RUNNING)))
            continue;

         if (ifa->ifa_flags & IFF_LOOPBACK)
            continue;

         family = ifa->ifa_addr->sa_family;
         if (family != AF_INET && family != AF_INET6)
            continue;

         if (family == AF_INET6 )
            continue;

         s = getnameinfo(ifa->ifa_addr,
               (family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6),
               host, NI_MAXHOST, 0, 0, NI_NUMERICHOST);
         if(s != 0) {
            //DEBUG(log,"failed to getnameinfo, error=%s", gai_strerror(s));
            continue;
         }
         if (!strcmp(host, "0.0.0.0") || !strcmp(host, "::") || !strncmp(host, "fe80:", 5))
            continue;

         // add interface to the ICE agent
         //DEBUG(log, "gather candidates, host=%s", host);
         address_init(&addr_local);
         if(address_set_from_string(&addr_local, host) != ICE_OK) {
            //ERROR(log,"Skipping invalid address, host=%s", host);
            continue;
         }
         ice_agent_add_local_address(session->agent, &addr_local);
         break;
      }
      freeifaddrs(ifaddr);
   }

   return 0;
}

int
ice_verify_stream_status(snw_ice_session_t *session) {
   snw_log_t *log = 0;
   int64_t before,now;

   if (!session) return -1;
   log = session->ice_ctx->log;

   before = session->lasttime;
   now = get_monotonic_time();

   //DEBUG(log, "time interval, delta=%lu, before=%lu, now=%lu",now-before,before,now);
   if (now-before >= ICE_USEC_PER_SEC) {
      if (session->audio_stream && session->audio_stream->rtp_component) {
         ERROR(log, "missing stream data, flowid=%u", session->flowid);
      }

      if(session->video_stream && session->video_stream->rtp_component) {
         ERROR(log, "missing stream data, flowid=%u", session->flowid);
      }
      before = now;
   }
   session->lasttime = now;

   return 0;
}

void 
send_rtcp_pkt_internal(snw_ice_session_t *session, int video, int encrypted, char *buf, int len) {
   static char sbuf[ICE_BUFSIZE];
   snw_log_t *log = 0;
   snw_ice_stream_t *stream = 0;
   snw_ice_component_t *component = 0;

   if (!session) return;
   log = session->ice_ctx->log;

   stream = IS_FLAG(session, WEBRTC_BUNDLE)
               ? (session->audio_stream ? session->audio_stream : session->video_stream)
               : (video ? session->video_stream : session->audio_stream);

   if (!stream) return;

   component = IS_FLAG(session, WEBRTC_RTCPMUX) ? stream->rtp_component : stream->rtcp_component;
   if (!component) return;

   //FIXME: check cdone equal to num of stream
   if (!stream->gathering_done) {
      return;
   }

   if (!component->dtls 
       || component->dtls->state != DTLS_STATE_CONNECTED 
       || !component->dtls->srtp_out) {
      return;
   }

   if (encrypted) {
      ice_agent_send(session->agent, stream->id, component->id,
                                 (const char *)buf, len);
   } else {
      int enc_len = 0;
      int ret = 0;

      memcpy(&sbuf, buf, len);
      enc_len = len;
      ret = srtp_protect_rtcp(component->dtls->srtp_out, &sbuf, &enc_len);
      if (ret != err_status_ok) {
         ERROR(log, "protect error, len=%d-->%d, s=%u", len, enc_len, ret);
      } else {
         ice_agent_send(session->agent, stream->id, component->id,
                                    (const char *)&sbuf, enc_len);
      }
   }
   return;
}

void
send_rtp_pkt_internal(snw_ice_session_t *session, 
  int video, int encrypted, char* buf, int len) {
   static char sbuf[ICE_BUFSIZE];
   snw_log_t *log = 0;
   snw_ice_stream_t *stream = 0;
   snw_ice_component_t *component = 0;
   rtp_hdr_t *header = (rtp_hdr_t *)&sbuf;
   int enc_len = len;
   int ret = 0;

   if (len > ICE_BUFSIZE) return;

   if (!session) return;
   log = session->ice_ctx->log;

   stream = IS_FLAG(session, WEBRTC_BUNDLE)
         ? (session->audio_stream ? session->audio_stream : session->video_stream)
         : (video ? session->video_stream : session->audio_stream);

   if (!stream) return;

   component = stream->rtp_component;
   if (!component || !stream->gathering_done) return;
   if (!component->dtls 
       || component->dtls->state != DTLS_STATE_CONNECTED
       || !component->dtls->srtp_out) {
      return;
   }

   memcpy(&sbuf, buf, len);
   header->ssrc = htonl(video ? stream->local_video_ssrc : stream->local_audio_ssrc);
   ret = srtp_protect(component->dtls->srtp_out, &sbuf, &enc_len);
   DEBUG(log, "SRTP protect, ret=%d , flowid=%u, len=%d-->%d",
         ret, session->flowid, len, enc_len);
   if (ret != err_status_ok) {
      uint32_t timestamp = ntohl(header->ts);
      uint16_t seq = ntohs(header->seq);
      ERROR(log, "srtp protect error, ret=%d , flowid=%u, len=%d-->%d, ts=%u, seq=%u",
            ret, session->flowid, len, enc_len, timestamp, seq);
      return;
   } 
   
   ret = ice_agent_send(session->agent, stream->id, component->id,
                                 (const char *)&sbuf, enc_len);
   if (ret < enc_len) {
      DEBUG(log, "only sent %d bytes? (was %d)", ret, enc_len);
   }

   //TODO: impl circular buffer to keep sent packets for restransmission.
   return;
}


void 
send_rtp_pkt(snw_ice_session_t *session, 
  int control, int video, char* buf, int len) {
   snw_log_t *log = 0;

   if (!session || !buf)
      return;
   log = session->ice_ctx->log;

   /* check status of receiver */
   ice_verify_stream_status(session);

   if (control) {
      send_rtcp_pkt_internal(session,video,0,buf,len);
   } else {
      send_rtp_pkt_internal(session,video,0,buf,len);
   }

   return;
}

void 
ice_relay_rtcp(snw_ice_session_t *session, int video, char *buf, int len) {
   snw_log_t *log = 0;
   if (!session || !buf || len < 1)
      return;
   log = session->ice_ctx->log;

   //FIXME: check stuff
   send_rtp_pkt(session,1,video,buf,len);
   return;
}

void 
snw_ice_handle_lost_packets(snw_ice_session_t *session, snw_ice_stream_t *stream,
      snw_ice_component_t *component, uint16_t seqno, int video) {
   snw_log_t *log = 0;
   uint32_t nack = 0;

   if (!session) return;
   log = session->ice_ctx->log;

   /* Save current seq number and 
      generate list of lost seqs */
   if (!video) {
      nack = snw_rtp_slidewin_put(session, &component->v_slidewin, seqno);
   }
    
   /* Generate NACK rtpfb message */
   if (nack != 0) {
      char rtcpbuf[RTCP_RTPFB_MSG_LEN];
      DEBUG(log,"sending rtpfb nack, flowid=%u, local_ssrc=%x, remote_ssrc=%x, payload=%x",
                              session->flowid,
                              stream->local_video_ssrc, 
                              stream->remote_video_ssrc, 
                              nack);
                            
         snw_rtcp_gen_nack(rtcpbuf, RTCP_RTPFB_MSG_LEN, 
                           stream->local_video_ssrc, 
                           stream->remote_video_ssrc, 
                           nack);
         send_rtcp_pkt_internal(session,video,0,rtcpbuf,RTCP_RTPFB_MSG_LEN);
   }

   return;
}

void
snw_ice_send_fir(snw_ice_session_t *session, snw_ice_component_t *component, int force) {
   snw_log_t *log = 0;
   snw_ice_stream_t *stream = 0;

   if (!session) return;
   log = session->ice_ctx->log;

   stream = IS_FLAG(session, WEBRTC_BUNDLE)
               ? (session->audio_stream ? session->audio_stream : session->video_stream)
               : session->video_stream;

   if (force || (component && (session->curtime - component->fir_latest > 10*ICE_USEC_PER_SEC))) {

      {// send fir command
         char rtcpbuf[RTCP_PSFB_FIR_MSG_LEN];
         component->fir_seq++;
         DEBUG(log,"sending fir request, flowid=%u, local_ssrc=%x, remote_ssrc=%x, fir_seq=%u",
                              session->flowid,
                              stream->local_video_ssrc, 
                              stream->remote_video_ssrc, 
                              component->fir_seq);
                            
         snw_rtcp_gen_fir(rtcpbuf, RTCP_PSFB_FIR_MSG_LEN, 
                           stream->local_video_ssrc, 
                           stream->remote_video_ssrc, 
                           component->fir_seq);
         send_rtcp_pkt_internal(session,1,0,rtcpbuf,RTCP_PSFB_FIR_MSG_LEN);
      }

      {// send pli report
         char rtcpbuf[RTCP_PSFB_PLI_MSG_LEN];
         DEBUG(log,"sending pli request, flowid=%u, local_ssrc=%x, remote_ssrc=%x, fir_seq=%u",
                              session->flowid,
                              stream->local_video_ssrc, 
                              stream->remote_video_ssrc, 
                              component->fir_seq);
         snw_rtcp_gen_pli(rtcpbuf, RTCP_PSFB_PLI_MSG_LEN,
                          stream->local_video_ssrc, 
                          stream->remote_video_ssrc);
         send_rtcp_pkt_internal(session,1,0,rtcpbuf,RTCP_PSFB_PLI_MSG_LEN);
      }

      //if (force) {
      if (1) {
         DEBUG(log, "vp8 payload sending fir request, cid=%u, curtime=%lu", 
                    component->id, session->curtime);
      }

      if (!force && component) {
         component->fir_latest = session->curtime;
      }
   }

   return;
}


void
ice_rtp_incoming_msg(snw_ice_session_t *session, snw_ice_stream_t *stream,
      snw_ice_component_t *component, char* buf, int len) {
   snw_log_t *log = 0;
   rtp_hdr_t *header = (rtp_hdr_t *)buf;
   err_status_t ret;
   uint32_t ssrc = 0;
   uint32_t timestamp = 0;
   uint16_t seq = 0;
   int buflen = len;
   int video = 0;

   if (!session || !stream || !component) return;
   log = session->ice_ctx->log;

   ssrc = ntohl(header->ssrc);
   timestamp = ntohl(header->ts);
   seq = ntohs(header->seq);
   /* XXX: stream ssrc should be set by sdp offer/answer? */
   if (ssrc != stream->remote_audio_ssrc
       && ssrc != stream->remote_video_ssrc) {
      ERROR(log, "wrong ssrc, ssrc=%u, ts=%u, seq=%u", ssrc, timestamp, seq);
      return;
   }
   video = ((stream->remote_video_ssrc == ssrc) ? 1 : 0);

   DEBUG(log, "rtp incoming message, flowid=%u, len=%u, video=%u, ssrc=%u, a_ssrc=%u, v_ssrc=%u",
              session->flowid, len, video, ssrc, stream->remote_audio_ssrc, stream->remote_video_ssrc);

   ret = srtp_unprotect(component->dtls->srtp_in, buf, &buflen);
   if (ret != err_status_ok) {
      ERROR(log, "unprotect error, len=%d, buflen=%d, ts=%u, seq=%u, res=%u",
            len, buflen, timestamp, seq, ret);
      return;
   } 

   if (IS_FLAG(session,ICE_PUBLISHER)) {
      snw_ice_broadcast_rtp_pkg(session,0,video,buf,buflen);
   } else if (IS_FLAG(session,ICE_SUBSCRIBER)) {
      //do nothing
   }

   //ice_rtp_plugin(); //FIXME: impl
   if (IS_FLAG(session,ICE_PUBLISHER)) {
      snw_ice_handle_lost_packets(session,stream,
          component,ntohs(header->seq),video);
      snw_ice_send_fir(session,component,0);
   }
   return;
}

int
snw_ice_resend_pkt(snw_ice_session_t *session, snw_ice_component_t *component,
              int video, int seqno, int64_t now) {
   snw_log_t *log = session->ice_ctx->log;

   DEBUG(log, "resend seq, flowid=%u, seqno=%u, ts=%llu",session->flowid, seqno, now);
   //FIXME: impl
   
   return 0;
}

void 
ice_rtcp_incoming_msg(snw_ice_session_t *session, snw_ice_stream_t *stream,
                          snw_ice_component_t *component, char* buf, int len) {
   snw_log_t *log = 0;
   err_status_t ret;
   uint32_t ssrc = 0;
   int buflen = len;
   int video = 0;

   if (!session) return;
   log = session->ice_ctx->log;

   ret = srtp_unprotect_rtcp(component->dtls->srtp_in, buf, &buflen);
   if (ret != err_status_ok) {
      DEBUG(log, "SRTCP unprotect error, ret=%u, len=%d, buflen=%d", ret, len, buflen);
      return;
   } 

   /* Determine which stream: audio or video? */
   ssrc = snw_rtcp_get_ssrc(session,buf,len);
   video = stream->remote_video_ssrc == ssrc ? 1 : 0;
   DEBUG(log, "ssrc info, flowid=%u, flags=%u, video=%u, ssrc=%u, buflen=%u",
           session->flowid, session->flags, video, ssrc, buflen);
   snw_stream_print_ssrc(session->ice_ctx, stream, "stream");
    
   /* Resend lost packets if having any */
   if (IS_FLAG(session,ICE_SUBSCRIBER))
      snw_rtcp_handle_nacks(session, component, video, buf, buflen, snw_ice_resend_pkt);
   
   return;
}

int ice_get_packet_type(snw_ice_session_t *session, char* buf, int len) {
   rtp_hdr_t *header = 0;
   
   if (!session || !buf || len <= 0) {
      return UNKNOWN_PT;
   }

   //FIXME: change number to CONSTANTS
   if ((*buf >= 20) && (*buf < 64)) {
      return DTLS_PT;
   }

   if (len < RTP_HEADER_SIZE) {
      return UNKNOWN_PT;
   }

   header = (rtp_hdr_t *)buf;
   if ((header->pt < 64) || (header->pt >= 96)) {
      return RTP_PT;
   } else if ((header->pt >= 64) && (header->pt < 96)) {
      return RTCP_PT;
   }

   return UNKNOWN_PT;
}


void ice_data_recv_cb(agent_t *agent, uint32_t stream_id,
          uint32_t component_id, char *buf, uint32_t len, void *data) {
   snw_log_t *log = 0;
   snw_ice_session_t *session = 0;
   snw_ice_stream_t *stream = 0;
   snw_ice_component_t *component = 0;
   int pt = 0;

   component = (snw_ice_component_t *)data;
   if (!component || !component->stream 
       || !component->stream->session) 
      return;

   stream = component->stream;
   session = stream->session;
   log = session->ice_ctx->log;
   session->curtime = get_monotonic_time();

   pt = ice_get_packet_type(session, buf,len);
   DEBUG(log, "get packet type, flowid=%u, pt=%u", 
              session->flowid, pt);
   if (pt == UNKNOWN_PT) {
      ERROR(log, "unknown packet type, flowid=%u, len=%u", 
                 session->flowid, len);
      return;
   }

   if (pt == DTLS_PT) {
      dtls_process_incoming_msg(component->dtls, buf, len);
      return;
   }

   if (!component->dtls 
       || component->dtls->state != DTLS_STATE_CONNECTED
       || !component->dtls->srtp_in) {
      WARN(log, "dtls not setup yet, flowid=%u", session->flowid);
   } else {
      if (pt == RTP_PT) {
         ice_rtp_incoming_msg(session,stream,component,buf,len);
      } else if (pt == RTCP_PT) {
         DEBUG(log, "rtcp pkt, flowid=%u", session->flowid);
         ice_rtcp_incoming_msg(session,stream,component,buf,len);
      }
   }

   return;
}


snw_ice_component_t*
snw_ice_create_media_component(snw_ice_session_t *session, snw_ice_stream_t *stream, uint32_t cid, int is_rtcp) {
   snw_ice_component_t *rtp = 0;

   if (!session) return 0;

   rtp = snw_component_allocate(session->ice_ctx);
   if (!rtp) return 0;

   rtp->stream = stream;
   rtp->id = cid;
   rtp->is_started = 0;
   INIT_LIST_HEAD(&rtp->remote_candidates.list);
   snw_component_insert(&stream->components, rtp);
   if (is_rtcp)
      stream->rtcp_component = rtp;
   else
      stream->rtp_component = rtp;

   //ice_agent_set_port_range(session->agent, stream->id, cid, rtp_range_min, rtp_range_max);
   return rtp;
}


int
snw_ice_create_media_stream(snw_ice_session_t *session, int video) {
   snw_log_t *log = 0;
   snw_ice_stream_t *stream = 0;
   snw_ice_component_t *rtp = 0;
   snw_ice_component_t *rtcp = 0;
   uint32_t stream_id;

   if (!session) return -1;
   log = session->ice_ctx->log;

   stream_id = ice_agent_add_stream(session->agent, IS_FLAG(session, WEBRTC_RTCPMUX) ? 1 : 2);
   stream = snw_stream_allocate(session->ice_ctx);
   if (stream == 0) {
      return -2;
   }
   stream->id = stream_id;
   stream->session = session;
   stream->gathering_done = 0;
   stream->is_disable = 0;
   stream->is_video = video;
   stream->dtls_type = DTLS_TYPE_ACTPASS;
   INIT_LIST_HEAD(&stream->components.list);
   snw_stream_insert(&session->streams,stream);
      
   if (video) {
      session->video_stream = stream;
      stream->local_video_ssrc = random();
      stream->remote_video_ssrc = 0;
      stream->local_audio_ssrc = 0;
      stream->remote_audio_ssrc = 0;
      DEBUG(log, "created video stream, sid=%u(%p)",
             session->video_stream->id, session->video_stream);
   } else {
      stream->local_audio_ssrc = random();
      stream->remote_audio_ssrc = 0;
      if (IS_FLAG(session, WEBRTC_BUNDLE)) {
         stream->local_video_ssrc = random();
         DEBUG(log, "generate video ssrc, ssrc=%u",stream->local_video_ssrc);
      } else {
         stream->local_video_ssrc = 0;
      }
      stream->remote_video_ssrc = 0;
      session->audio_stream = stream;
   }

   rtp = snw_ice_create_media_component(session,stream,1,0);
   if (rtp == 0) {
      return -3;
   }

   if (!IS_FLAG(session, WEBRTC_RTCPMUX)) {
      rtcp = snw_ice_create_media_component(session,stream,2,1);
      if(rtcp == 0) {
         return -3;
      }
   }

   /*{//DEBUG
      struct list_head *n;
      list_for_each(n,&session->streams.list) {
         ice_stream_t *s = list_entry(n,ice_stream_t,list);
         DEBUG("view stream, s-sid=%u(%p)", s->id, s);
      }
   }*/

   DEBUG(log, "initialize media stream, video=%u, stream_id=%u(%p), %p", 
              video, stream_id, stream, &session->streams);
   ice_agent_gather_candidates(session->agent, stream_id);
   ice_agent_attach_recv(session->agent, stream_id, 1, ice_data_recv_cb, rtp);
   if (!IS_FLAG(session, WEBRTC_RTCPMUX) && rtcp != 0)
      ice_agent_attach_recv(session->agent, stream_id, 2, ice_data_recv_cb, rtcp);

   return 0;
}

int
snw_ice_session_setup(snw_ice_context_t *ice_ctx, snw_ice_session_t *session, int offer, char *sdp) {
   snw_log_t *log = 0;
   agent_t *agent;
   ice_sdp_attr_t sdp_attr;
   int ret = 0; 

   if (!ice_ctx || !session || !sdp) return -1;
   log = ice_ctx->log;

   agent = (agent_t*)ice_agent_new(session->base,ICE_COMPATIBILITY_RFC5245,0);
   if (!agent) return -1;


   // set callbacks and handlers for ice protocols
   ice_set_candidate_gathering_done_cb(agent, snw_ice_cb_candidate_gathering_done, session);
   ice_set_new_selected_pair_cb(agent, snw_ice_cb_new_selected_pair, session);
   ice_set_component_state_changed_cb(agent, snw_ice_cb_component_state_changed, session);
   ice_set_new_remote_candidate_cb(agent, snw_ice_cb_new_remote_candidate, session);

   session->ice_ctx = ice_ctx;
   session->agent = agent;
   session->streams_gathering_done = 0;
   session->streams_num = 0;
   session->control_mode = ICE_CONTROLLED_MODE;

   DEBUG(log,"Creating ICE agent, flowid=%u, ice_lite=%u, control_mode=%u, sdp_len=%u",
         session->flowid, ice_ctx->ice_lite_enabled, session->control_mode, strlen(sdp));

   ret = snw_ice_add_local_addresses(session);
   if (ret < 0) {
      //FIXME: clean resources
      return -2;
   }

   //FIXME: rewrite this code
   snw_ice_get_sdp_attr(ice_ctx,sdp,&sdp_attr);
   DEBUG(log,"Setting ICE locally: offer=%u, audio=%u, video=%u,"
         " budnle=%u, rtcpmux=%u, trickle=%u", 
         offer, sdp_attr.audio, sdp_attr.video, 
         sdp_attr.bundle, sdp_attr.rtcpmux, sdp_attr.trickle);

   if (sdp_attr.audio) {
      SET_FLAG(session, WEBRTC_AUDIO);
   } else {
      CLEAR_FLAG(session, WEBRTC_AUDIO);
   }

   if (sdp_attr.video) {
      SET_FLAG(session, WEBRTC_VIDEO);
   } else {
      CLEAR_FLAG(session, WEBRTC_VIDEO);
   }

   if (sdp_attr.audio) {
      session->streams_num++;
   }

   if (sdp_attr.video && (!sdp_attr.audio || !IS_FLAG(session, WEBRTC_BUNDLE))) {
      session->streams_num++;
   }

   DEBUG(log, "Checking media stream, offer=%u, audio=%u, video=%u, streams_num=%u, flags=%u",
         offer,sdp_attr.audio,sdp_attr.video,session->streams_num, IS_FLAG(session, WEBRTC_BUNDLE));
   
   if (IS_FLAG(session, WEBRTC_AUDIO)) { 
      ret = snw_ice_create_media_stream(session,0);
      if (ret < 0) {
         ERROR(log, "ret=%d", ret);
         return ret;
      }
   }

   if (sdp_attr.video && (!sdp_attr.audio || !IS_FLAG(session, WEBRTC_BUNDLE))) {
      ret = snw_ice_create_media_stream(session,1);
      if (ret < 0) {
         ERROR(log, "ret=%d", ret);
         return ret;
      }
   }

   return 0;
}


//video_offer_sdp
static int
snw_ice_offer_sdp(snw_ice_context_t *ice_ctx, 
      snw_ice_session_t *session, uint32_t flowid, int sendonly) {
   static const char *sdp_template = 
         "v=0\r\no=- %lu %lu IN IP4 127.0.0.1\r\ns=%s\r\nt=0 0\r\n%s%s";
   static  const char *audio_mline_template = 
         "m=audio 1 RTP/SAVPF %d\r\nc=IN IP4 1.1.1.1\r\na=%s\r\na=rtpmap:%d opus/48000/2\r\n";
   static  const char *video_mline_template = 
         "m=video 1 RTP/SAVPF %d\r\nc=IN IP4 1.1.1.1\r\na=%s\r\na=rtpmap:%d VP8/90000\r\n"
         "a=rtcp-fb:%d ccm fir\r\na=rtcp-fb:%d nack\r\na=rtcp-fb:%d nack pli\r\na=rtcp-fb:%d goog-remb\r\n";
   static char sdp[1024], audio_mline[256], video_mline[512];
   snw_log_t *log = 0;
   int ret = 0;

   if (!ice_ctx || !session) return -1;
   log = ice_ctx->log;

   DEBUG(log,"sendonly=%u",sendonly);

   memset(audio_mline,0,512);
   snprintf(audio_mline, 256, audio_mline_template,
       OPUS_PT, sendonly ? "sendonly" : "sendrecv", OPUS_PT);

   memset(video_mline,0,512);
   snprintf(video_mline, 512, video_mline_template,
       VP8_PT, sendonly ? "sendonly" : "sendrecv",
       VP8_PT, VP8_PT, VP8_PT, VP8_PT, VP8_PT);

   memset(sdp,0,1024);
   snprintf(sdp, 1024, sdp_template,
       get_real_time(), get_real_time(),
       "PeerCall Replay", audio_mline, video_mline);

   ret = snw_ice_session_setup(ice_ctx, session, 0, (char *)sdp);
   if (ret < 0) {
      ERROR(log, "Error setting ICE locally, ret=%d",ret);
      return -4;
   }

   return 0;
}

//video_start_handler
void
snw_ice_connect_msg(snw_ice_context_t *ice_ctx, Json::Value &root, uint32_t flowid) {
   snw_context_t *ctx = (snw_context_t*)ice_ctx->ctx;
   snw_log_t *log = ice_ctx->log;
   snw_ice_session_t *session;
   snw_ice_channel_t *channel;
   int is_new = 0;
   uint32_t channelid;
   
   try {
      channelid = root["channelid"].asUInt();
   } catch (...) {
      ERROR(log, "json format error");
      return;
   }

   session = (snw_ice_session_t*)snw_ice_session_get(ice_ctx,flowid,&is_new);
   if (!session) {
      ERROR(log,"failed to get session, flowid=%u",flowid);
      return;
   }

   if (!is_new) {
      ERROR(log,"old session, flowid=%u, ice_ctx=%p",session->flowid, session->ice_ctx);
      return;
   }

   DEBUG(log,"init new session, channelid=%u, flowid=%u", channelid, session->flowid);
   session->channelid = channelid;
   session->control_mode = ICE_CONTROLLED_MODE;
   session->base = ctx->ev_base;
   session->flags = 0;
   INIT_LIST_HEAD(&session->streams.list);
   DEBUG(log,"search channel, flowi=%u, channelid=%u",flowid,channelid);
   channel = (snw_ice_channel_t*)snw_ice_channel_search(ice_ctx,channelid);
   if (!channel) {
      ERROR(log,"channel not found, flowid=%u, channleid=%u",flowid,channelid);
      return;
   }
   session->channel = channel;

   snw_ice_offer_sdp(ice_ctx,session,flowid,0);
   return;
}

void 
ice_component_cleanup(snw_ice_context_t *ice_ctx, snw_ice_component_t *component) {

   if (!component) return;

   if (component->dtls != 0) {
      dtls_free(component->dtls);
      component->dtls = 0;
   }
   
   snw_component_deallocate(ice_ctx, component);
   return;
}

void
ice_component_free(snw_ice_context_t *ice_ctx, snw_ice_component_t *components, 
      snw_ice_component_t *component) {
   snw_log_t *log = 0;
   struct list_head *pos,*n;
   snw_ice_component_t *c = 0;

   if (!components || !component)
      return;
   log = ice_ctx->log;
 
   list_for_each_safe(pos,n,&components->list) {
      snw_ice_component_t *t = list_entry(pos,snw_ice_component_t,list);
      if (t->id == component->id) {
         DEBUG(log, "remove component, cid=%u",t->id);
         list_del(pos);
         c = t;
      }
   }

   if (c) {
      ice_component_cleanup(ice_ctx,c);
   } else {
      //ERROR("component not found, cid=%u", component->id);
   }

   return;
}


void 
ice_stream_cleanup(snw_ice_context_t *ice_ctx, snw_ice_stream_t *stream) {
   snw_log_t *log = 0;

   if (!stream)
      return;
   log = ice_ctx->log;   

   //FIXME: delete components
   DEBUG(log, "stream cleanup, sid=%u",stream->id);
   if (stream->rtp_component != 0) {
      ice_component_free(ice_ctx, &stream->components, stream->rtp_component);
   }

   if (stream->rtcp_component != 0) {
      ice_component_free(ice_ctx, &stream->components, stream->rtcp_component);
   }

   snw_stream_deallocate(ice_ctx,stream);

   return;
}

void 
ice_stream_free(snw_ice_context_t *ice_ctx, snw_ice_stream_t *streams, snw_ice_stream_t *stream) {
   snw_log_t *log = 0;
   struct list_head *pos,*n;
   snw_ice_stream_t *d = 0; 
         
   if (!streams || !stream)
      return;
   log = ice_ctx->log;

   {//DEBUG
      struct list_head *n;
      list_for_each(n,&streams->list) {
         snw_ice_stream_t *s = list_entry(n,snw_ice_stream_t,list);
         DEBUG(log,"view stream, sid=%u(%p)", s->id, s);
      }
   }

   list_for_each_safe(pos,n,&streams->list) {
      snw_ice_stream_t *s = list_entry(pos,snw_ice_stream_t,list);
      if ( s->id == stream->id ) {
         //DEBUG("delete stream, sid=%u",s->id);
         list_del(pos);
         d = s;
      }
   }

   if (d) {
      ice_stream_cleanup(ice_ctx, d);
   } else {
      ERROR(log, "stream not found, sid=%u", stream->id);
   }

   return;
}

void 
snw_ice_session_free(snw_ice_context_t *ice_ctx, snw_ice_session_t *session) {
   snw_log_t *log = 0;
   struct list_head *n, *p;

   if (!session || !ice_ctx) return;
   log = ice_ctx->log;


   if (session->agent) {
      ice_agent_free(session->agent);
      session->agent = 0;
   }

   if (session->local_sdp) {
      free(session->local_sdp);
      session->local_sdp = 0;
   }

   if (session->remote_sdp) {
      free(session->remote_sdp);
      session->remote_sdp = 0;
   }

   //FIXME: free streams & components
   list_for_each_safe(n,p,&session->streams.list) {
      snw_ice_stream_t *s = list_entry(n,snw_ice_stream_t,list);
      DEBUG(log, "delete stream, sid=%u(%p)", s->id, s);
      list_del(&s->list);
      //ice_stream_free(session->ice_ctx,&session->streams,s);
      ice_stream_cleanup(session->ice_ctx,s);
   }

   if (session->live_channelid) {
      snw_ice_channel_t *channel = 0;
      DEBUG(log, "search channel, flowid=%u, channelid=%u", session->flowid, session->live_channelid);
      channel = (snw_ice_channel_t*)snw_ice_channel_search(ice_ctx,session->live_channelid);
      if (!channel) return;
      snw_print_channel_info(ice_ctx,channel); 
      for (int i=0; i<SNW_ICE_CHANNEL_USER_NUM_MAX; i++) {
         if (channel->players[i] == session->flowid) {
            channel->players[i] = 0;
            DEBUG(log, "found subscriber, flowid=%u, channelid=%u", 
                  session->flowid, session->live_channelid);
            snw_print_channel_info(ice_ctx,channel); 
            break;
         }
      }
   }

   CLEAR_FLAG(session, WEBRTC_READY);
   DEBUG(log, "WebRTC resources freed, flowid=%u", session->flowid);
   return;
}

void
snw_ice_stop_msg(snw_ice_context_t *ice_ctx, Json::Value &root, uint32_t flowid) {
   snw_log_t *log = ice_ctx->log;
   snw_ice_session_t *session;
   
   session = (snw_ice_session_t*)snw_ice_session_search(ice_ctx,flowid);
   if (!session) {
      WARN(log,"session not found, flowid=%u",flowid);
      return;
   }

   WARN(log,"stop session, flowid=%u",flowid);
   snw_ice_session_free(ice_ctx,session);
   return;
}

int
snw_ice_merge_streams(snw_ice_session_t *session, int audio, int video) {
   snw_log_t *log = 0;

   if (!session) return -1;
   log = session->ice_ctx->log;

   DEBUG(log, "remove unneccessary RTP components, audio=%u,video=%u",audio,video);
   if (audio) {
      if( !list_empty(&session->streams.list) && session->video_stream) {
         session->audio_stream->local_video_ssrc = session->video_stream->local_video_ssrc;
         session->audio_stream->remote_video_ssrc = session->video_stream->remote_video_ssrc;
         ice_agent_attach_recv(session->agent, session->video_stream->id, 1, 0, 0);
         ice_agent_attach_recv(session->agent, session->video_stream->id, 2, 0, 0);
         ice_agent_remove_stream(session->agent, session->video_stream->id);
         ice_stream_free(session->ice_ctx,&session->streams, session->video_stream);
      }
      session->video_stream = 0;
   } else if (video) {
      //FIXME: what to do?
   }

   return 0;
}  

int
snw_ice_merge_components(snw_ice_session_t *session) {
   snw_log_t *log = 0;

   if (!session) return -1;
   log = session->ice_ctx->log;

   DEBUG(log, "removing unneccessary rtcp components");
   if(session->audio_stream && !list_empty(&session->audio_stream->components.list) ) {
      ice_agent_attach_recv(session->agent, session->audio_stream->id, 2, 0, 0);
      ice_component_free(session->ice_ctx, &session->audio_stream->components, 
            session->audio_stream->rtcp_component);
      session->audio_stream->rtcp_component = 0;
   }

   if(session->video_stream && !list_empty(&session->video_stream->components.list)) {
      ice_agent_attach_recv(session->agent, session->video_stream->id, 2, 0, 0);
      ice_component_free(session->ice_ctx, &session->video_stream->components, 
           session->video_stream->rtcp_component);
      session->video_stream->rtcp_component = 0;
   }

   return 0;
}


int ice_setup_remote_credentials(snw_ice_session_t *session, snw_ice_stream_t *stream, snw_ice_component_t *component) {
   snw_log_t *log = 0;
   candidate_t *c = 0;
   struct list_head *gsc,*n;
   char *ufrag = 0, *pwd = 0;

   if (!session) return -1;
   log = session->ice_ctx->log;

   /* FIXME: make sense? */
   list_for_each_safe(gsc,n,&component->remote_candidates.list) {
      c = list_entry(gsc,candidate_t,list);
      DEBUG(log, "remote stream info, sid=%d, cid=%d", c->stream_id, c->component_id);
      if (c->username && !ufrag)
         ufrag = c->username;
      if (c->password && !pwd)
         pwd = c->password;

      //PRINT_CANDIDATE(c);
      if (address_is_private(&(c->addr)) ) {
         char address[ICE_ADDRESS_STRING_LEN];
         address_to_string(&(c->addr), (char *)&address);
         DEBUG(log, "not removing private ip, ip=%s",address);
         //list_del(&c->list);
         /* FIXME: removing private ips causes failure of ICE process */
      }
   }

   if (ufrag && pwd) {
      DEBUG(log, "setting remote credentials, ufrag=%s,pwd=%s",ufrag,pwd);
      if (ice_agent_set_remote_credentials(session->agent, stream->id, ufrag, pwd) != ICE_OK) {
         DEBUG(log, "failed to set remote credentials, sid=%u, cid=%u",stream->id, component->id);
         return -1;
      }
   }

   return 0;
}

void 
ice_setup_remote_candidates(snw_ice_session_t *session, uint32_t stream_id, uint32_t component_id) {
   snw_log_t *log = 0;
   snw_ice_stream_t *stream = 0;
   snw_ice_component_t *component = 0;
   int added = 0;
   
   if (!session || !session->agent || list_empty(&session->streams.list))
      return;
   log = session->ice_ctx->log;

   stream = snw_stream_find(&session->streams, stream_id);
   if (!stream || list_empty(&stream->components.list)) {
      ERROR(log, "stream not found, sid=%d, cid=%d", stream_id, component_id);
      return;
   }  
      
   if (stream->is_disable) {
      ERROR(log, "stream info, disabled=%u, sid=%u, cid=%u", 
            stream->is_disable, stream_id, component_id);
      return;
   }     
      
   component = snw_component_find(&stream->components, component_id);
   if (!component) {
      ERROR(log, "component not found, sid=%u, cid=%u", stream_id, component_id);
      return;
   }

   if(component->is_started) {
      DEBUG(log, "component started, sid=%u, cid=%u", stream_id, component_id);
      return;
   }

   if(list_empty(&component->remote_candidates.list)) {
      DEBUG(log, "candidate list is empty");
      return;
   }
   DEBUG(log, "Setting credentials of remote candidates, sid=%d, cid=%d", stream_id, component_id);
   ice_setup_remote_credentials(session,stream,component);

   added = ice_agent_set_remote_candidates(session->agent, stream_id, 
                component_id, &component->remote_candidates);
   if(added <=  0 ) { //FIXME: compare to size of list candidates
      DEBUG(log, "failed to set remote candidates, added=%u", added);
   } else {
      DEBUG(log, "remote candidates set, added=%u",added);
      component->is_started = 1;
   }

   return;
}


int
try_ice_start(snw_ice_session_t *session) {
   snw_log_t *log = 0;

   if (!session) return -1;
   log = session->ice_ctx->log;

   //process_pending_trickles(session);

   if (IS_FLAG(session, WEBRTC_TRICKLE) && !IS_FLAG(session, WEBRTC_GATHER_DONE)) {
      DEBUG(log, "webrtc start with trickle");
      SET_FLAG(session, WEBRTC_START);
   } else {
      /* FIXME: never reach here */
      DEBUG(log, "Sending connectivity checks, audio_id=%u,video_id=%u",
             session->audio_stream->id, session->video_stream->id);
      if (session->audio_stream->id > 0) {
         ice_setup_remote_candidates(session, session->audio_stream->id, 1);
         if(!IS_FLAG(session, WEBRTC_RTCPMUX))  {
            /* section-5.1.3 in rfc5761 */
            ice_setup_remote_candidates(session, session->audio_stream->id, 2);
         }
      }
      if (session->video_stream->id > 0) {
         ice_setup_remote_candidates(session, session->video_stream->id, 1);
         if(!IS_FLAG(session, WEBRTC_RTCPMUX))
            /* section-5.1.3 in rfc5761 */
            ice_setup_remote_candidates(session, session->video_stream->id, 2);
      }
   }

   return 0;
}


void
snw_ice_sdp_msg(snw_ice_context_t *ice_ctx, Json::Value &root, uint32_t flowid) {
   snw_context_t *ctx = (snw_context_t*)ice_ctx->ctx;
   snw_log_t *log = ice_ctx->log;
   snw_ice_session_t *session;
   ice_sdp_attr_t sdp_attr;
   Json::Value type,jsep,jsep_trickle,sdp;
   Json::FastWriter writer;
   const char *jsep_type = 0;
   char *jsep_sdp = 0;
   //sdp_parser_t *sdp_parser = 0;
   std::string output;
   int ret = 0;

   session = (snw_ice_session_t*)snw_ice_session_search(ice_ctx, flowid);
   if (session == 0) {
      ERROR(log, "failed to malloc");
      return;
   }

   try {

      jsep = root["sdp"];
      if (!jsep.isNull()) {
         type = jsep["type"];
         jsep_type = type.asString().c_str();
         DEBUG(log, "get sdp type, type=%s",jsep_type);
      } else {
         output = writer.write(root);
         ERROR(log, "failed to get sdp type, root=%s",output.c_str());
         goto jsondone;
      }

      if (!strcasecmp(jsep_type, "answer")) {
         // only handle answer
         DEBUG(log, "got sdp answer, answer=%s",jsep_type);
      } else if(!strcasecmp(jsep_type, "offer")) {
         ERROR(log, "not handling offer, type=%s", jsep_type);
         goto jsondone;
      } else {
         ERROR(log, "unknown message type, type=%s", jsep_type);
         goto jsondone;
      }
      sdp = jsep["sdp"];
      if (sdp.isNull() || !sdp.isString() ) {
         ERROR(log, "sdp not found");
         goto jsondone;
      }

      jsep_sdp = strdup(sdp.asString().c_str()); //FIXME: don't use strdup
      DEBUG(log, "Remote SDP, trickle=%u, s=%s", sdp_attr.trickle, jsep_sdp);

      ret = snw_ice_get_sdp_attr(ice_ctx,jsep_sdp,&sdp_attr);
      if (ret < 0) {
         ERROR(log, "invalid sdp, sdp=%s",jsep_sdp);
         goto jsondone;
      }

      if (!IS_FLAG(session, WEBRTC_READY)) {
         session->remote_sdp = strdup(jsep_sdp);
         snw_ice_sdp_handle_answer(session, jsep_sdp);//sdp_parser);

         DEBUG(log, "setting webrtc flags, bundle=%u,rtcpmux=%u,trickle=%u",
                  sdp_attr.bundle,sdp_attr.rtcpmux,sdp_attr.trickle);
         if (sdp_attr.bundle) {
            SET_FLAG(session, WEBRTC_BUNDLE);
            snw_ice_merge_streams(session,sdp_attr.audio,sdp_attr.video);
         } else {
            CLEAR_FLAG(session, WEBRTC_BUNDLE);
         }

         if (sdp_attr.rtcpmux) {
            SET_FLAG(session, WEBRTC_RTCPMUX);
            snw_ice_merge_components(session);
         } else {
            CLEAR_FLAG(session, WEBRTC_RTCPMUX);
         }

         if (sdp_attr.trickle) {
            SET_FLAG(session, WEBRTC_TRICKLE);
         } else {
            CLEAR_FLAG(session, WEBRTC_TRICKLE);
         }

         //FIXME: rewrite it, move to sdp_attr_parsing
         verify_disabled_streams(session,sdp_attr.audio,sdp_attr.video,jsep_sdp);

         try_ice_start(session);

      } else {
         ERROR(log, "state error, flags=%u",session->flags);
         goto jsondone;
      }

      root["rc"] = 0;
      output = writer.write(root);
      DEBUG(log, "Sending result to client, result=%s",output.c_str());
      //enqueue_msg_to_mcd(output.c_str(),output.size(),flowid);
      snw_shmmq_enqueue(ctx->snw_ice2core_mq,0,output.c_str(),output.size(),flowid);
   } catch (...) {
      root["rc"] = -1;
      output = writer.write(root);
      DEBUG(log, "json format error, root=%s",output.c_str());
      //enqueue_msg_to_mcd(output.c_str(),output.size(),flowid);
      snw_shmmq_enqueue(ctx->snw_ice2core_mq,0,output.c_str(),output.size(),flowid);
   }

jsondone:
   if (jsep_sdp)
      free(jsep_sdp);
   return;
}
int ice_sdp_handle_candidate(snw_ice_stream_t *stream, const char *candidate) {
   snw_log_t *log = 0;
   snw_ice_session_t *session = 0;
   snw_ice_component_t *component = 0;
   candidate_t *c = 0;
   char foundation[16], transport[4], type[6]; 
   char ip[32], relip[32];
   uint32_t component_id, priority, port, relport;
   int ret;

   if (stream == 0 || candidate == 0)
      return -1; 

   session = stream->session;
   if (session == 0)
      return -2; 
   log = session->ice_ctx->log;

   if (strstr(candidate, "candidate:") == candidate) {
      candidate += strlen("candidate:");
   }   

   /* format: foundation component tranpsort priority ip port type ??? ??? ??? ??? */
   ret = sscanf(candidate, "%15s %30u %3s %30u %31s %30u typ %5s %*s %31s %*s %30u",
                           foundation, &component_id, transport, &priority,
                           ip, &port, type, relip, &relport);

   DEBUG(log, "parsing result, ret=%u, cid:%d sid:%d, type:%s, transport=%s, refaddr=%s:%d, addr=%s:%d",
         ret, component_id, stream->id, type, transport, relip, relport, ip, port);

   if (ret >= 7) {
      component = snw_component_find(&stream->components, component_id);
      if (component == 0) {
         ERROR(log, "component not found, cid=%u, sid=%u", component_id, stream->id);
         return -3; 
      }   
         DEBUG(log, "component found, cid=%u, sid=%u", component_id, stream->id);
      c = snw_ice_remote_candidate_new(type,transport);
      if (c != 0) {
         DEBUG(log, "new candidate, cid=%u, sid=%u", component_id, stream->id);
         c->component_id = component_id;
         c->stream_id = stream->id;

         if (!strcasecmp(transport, "udp")) {
            c->transport = ICE_CANDIDATE_TRANSPORT_UDP;
         } else {
            /* FIXME: support other transport, see secion-4.5 in rfc6544 */
            candidate_free(c);
            return -4;
         }

         strncpy(c->foundation, foundation, ICE_CANDIDATE_MAX_FOUNDATION);
         c->priority = priority;
         address_set_from_string(&c->addr, ip);
         address_set_port(&c->addr, port);
         c->username = strdup(stream->remote_user);
         c->password = strdup(stream->remote_pass);
         address_set_from_string(&c->base_addr, relip);
         address_set_port(&c->base_addr, relport);
         /* FIXME: new candidate is not free when component->is_started = 0*/
         snw_ice_try_start_component(session,stream,component,c);
      }
   } else {
      ERROR(log, "failed to parse candidate, ret=%d, s=%s", ret, candidate);
      return ret;
   }
   return 0;
}

int 
snw_ice_process_new_candidate(snw_ice_session_t *session, Json::Value &candidate) {
   snw_log_t *log = 0;
   Json::Value mid, mline, rc, done;
   snw_ice_stream_t *stream = 0;
   int video = 0;
   int ret = 0;

   if (!session) return -1;
   log = session->ice_ctx->log;

   done = candidate["done"];
   if (!done.isNull()) {
      DEBUG(log, "No more remote candidates");
      SET_FLAG(session, WEBRTC_GATHER_DONE);
      return 0;
   }

   mid = candidate["id"];
   if (mid.isNull() || !mid.isString()) {
      return -2;
   }

   mline = candidate["label"];
   if (mline.isNull()|| !mline.isInt() || mline.asInt() < 0) {
      return -3;
   }

   rc = candidate["candidate"];
   if (rc.isNull() || !rc.isString()) {
      return -4;
   }

   DEBUG(log, "remote candidate, mid=%s, candidate=%s", mid.asString().c_str(), rc.asString().c_str());
   if ( !strncasecmp(mid.asString().c_str(),"video",5) ) {
      video = 1;
   }

   stream = video ? session->video_stream : session->audio_stream;
   if(stream == 0) {
      return -5;
   }

   ret = ice_sdp_handle_candidate(stream, rc.asString().c_str());
   if(ret != 0) {
      ERROR(log, "failed to handle candidate, ret=%d)", ret);
      return -6;
   }

   return 0;
}


void
snw_ice_candidate_msg(snw_ice_context_t *ice_ctx, Json::Value &root, uint32_t flowid) {
   snw_context_t *ctx = (snw_context_t*)ice_ctx->ctx;
   snw_log_t *log = ice_ctx->log;
   snw_ice_session_t *session;
   Json::Value candidate;
   Json::FastWriter writer;
   std::string output;

   session = (snw_ice_session_t*)snw_ice_session_search(ice_ctx, flowid);
   if (!session) {
      DEBUG(log, "ice session is 0");
      return;
   }

   try {
      candidate = root["candidate"];

      output = writer.write(candidate);
      DEBUG(log, "receive candidate, s=%s",output.c_str());

      if (!IS_FLAG(session, WEBRTC_TRICKLE)) {
         DEBUG(log, "supports trickle even if it didn't negotiate it");
         SET_FLAG(session, WEBRTC_TRICKLE);
      }    
     
      if (!session->audio_stream && !session->video_stream) {
         /* FIXME: save trickle candidate. */
         return;
      }    
 
      if ( !candidate.isNull() ) {
         int ret = 0; 
         if ((ret = snw_ice_process_new_candidate(session, candidate)) != 0) { 
            DEBUG(log, "got error, ret=%d", ret);
            return;
         }    
      } else {
         ERROR(log, "candidate is null");
      }    


      root["rc"] = 0; 
      output = writer.write(root);
      snw_shmmq_enqueue(ctx->snw_ice2core_mq,0,output.c_str(),output.size(),flowid);

   } catch (...) {
      root["rc"] = -1;
      output = writer.write(root);
      DEBUG(log, "json format error, root=%s",output.c_str());
      snw_shmmq_enqueue(ctx->snw_ice2core_mq,0,output.c_str(),output.size(),flowid);
   }



   return;
}

void
snw_ice_publish_msg(snw_ice_context_t *ice_ctx, Json::Value &root, uint32_t flowid) {
   snw_log_t *log = 0;
   snw_ice_session_t *session = 0;

   if (!ice_ctx) return;
   log = ice_ctx->log;

   // get session
   session = (snw_ice_session_t*)snw_ice_session_search(ice_ctx, flowid);
   if (!session) return;
   
   // update session
   DEBUG(log, "channel is published, flowid=%u, channelid=%u", 
         flowid, session->channelid);
   SET_FLAG(session,ICE_PUBLISHER);

   // start broadcasting session
   snw_print_channel_info(ice_ctx,session->channel); 
   
   return;
}

void
snw_ice_play_msg(snw_ice_context_t *ice_ctx, Json::Value &root, uint32_t flowid) {
   snw_log_t *log = 0;
   snw_ice_session_t *session = 0;
   uint32_t channelid = 0;

   if (!ice_ctx) return;
   log = ice_ctx->log;

   session = (snw_ice_session_t*)snw_ice_session_search(ice_ctx, flowid);
   if (!session) return;
   SET_FLAG(session,ICE_SUBSCRIBER);
   
   try {
      channelid = root["channelid"].asUInt();
   } catch (...) {
      ERROR(log, "json format error");
   }
   snw_channel_add_subscriber(ice_ctx, channelid, flowid);
   session->live_channelid = channelid;

  
   return;
}

void
snw_ice_fir_msg(snw_ice_context_t *ice_ctx, Json::Value &root, uint32_t flowid) {
   snw_log_t *log = 0;
   log = ice_ctx->log;
   DEBUG(log, "FIXME fir msg");
   return;
}

void
snw_ice_process_msg(snw_ice_context_t *ice_ctx, char *data, uint32_t len, uint32_t flowid) {
   snw_log_t *log = ice_ctx->log;
   Json::Value root;
   Json::Reader reader;
   Json::FastWriter writer;
   std::string output;
   uint32_t msgtype = 0, api = 0;
   int ret;

   ret = reader.parse(data,data+len,root,0);
   if (!ret) {
      ERROR(log,"error json format, s=%s",data);
      return;
   }

   DEBUG(log, "get ice msg, flowid=%u, data=%s", flowid, data);
   try {
      msgtype = root["msgtype"].asUInt();
      if (msgtype != SNW_ICE) {
         ERROR(log, "wrong msg, msgtype=%u data=%s", msgtype, data);
         return;
      }
      api = root["api"].asUInt();
   } catch (...) {
      ERROR(log, "json format error, data=%s", data);
   }

   switch(api) {
      case SNW_ICE_CREATE:
         snw_ice_create_msg(ice_ctx,root,flowid);
         break;
      case SNW_ICE_CONNECT:
         snw_ice_connect_msg(ice_ctx,root,flowid);
         break;
      case SNW_ICE_STOP:
         snw_ice_stop_msg(ice_ctx,root,flowid);
         break;
      case SNW_ICE_SDP:
         snw_ice_sdp_msg(ice_ctx,root,flowid);
         break;
      case SNW_ICE_CANDIDATE:
         snw_ice_candidate_msg(ice_ctx,root,flowid);
         break;
      case SNW_ICE_PUBLISH:
         snw_ice_publish_msg(ice_ctx,root,flowid);
         break;
      case SNW_ICE_PLAY:
         snw_ice_play_msg(ice_ctx,root,flowid);
         break;
      case SNW_ICE_FIR:
         snw_ice_fir_msg(ice_ctx,root,flowid);
         break;

      default:
         ERROR(log, "unknow api, api=%u", api);
         break;
   }

   return;
}


