# Milan ACMP Listener Sink

![diagram](milan_acmp_sink_sm.svg)


**Implements:** Milan v1.3 Section 5.5.3

| Event | Unbound | PrbWAvail | PrbWDelay | PrbWResp | PrbWResp2 | PrbWRetry | SettledNoRsv | SettledRsvOk |
|-------|--------|--------|--------|--------|--------|--------|--------|--------|
| StartBound | start_bound()<br/>-> PrbWAvail | -x- | -x- | -x- | -x- | -x- | -x- | -x- |
| BindRefused | refuse_command()<br/>-> Unbound | refuse_command()<br/>-> PrbWAvail | refuse_command()<br/>-> PrbWDelay | refuse_command()<br/>-> PrbWResp | refuse_command()<br/>-> PrbWResp2 | refuse_command()<br/>-> PrbWRetry | refuse_command()<br/>-> SettledNoRsv | refuse_command()<br/>-> SettledRsvOk |
| BindSameTalker | -x- | update_binding()<br/>-> PrbWAvail | update_binding()<br/>-> PrbWDelay | update_binding()<br/>-> PrbWResp | update_binding()<br/>-> PrbWResp2 | update_binding()<br/>-> PrbWRetry | update_binding()<br/>-> SettledNoRsv | update_binding()<br/>-> SettledRsvOk |
| BindNewTalker | bind_and_probe()<br/>-> PrbWResp | bind_and_probe()<br/>-> PrbWResp | bind_and_probe()<br/>-> PrbWResp | bind_and_probe()<br/>-> PrbWResp | bind_and_probe()<br/>-> PrbWResp | bind_and_probe()<br/>-> PrbWResp | bind_and_probe()<br/>-> PrbWResp | bind_and_probe()<br/>-> PrbWResp |
| GetRxState | answer_rx_state()<br/>-> Unbound | answer_rx_state()<br/>-> PrbWAvail | answer_rx_state()<br/>-> PrbWDelay | answer_rx_state()<br/>-> PrbWResp | answer_rx_state()<br/>-> PrbWResp2 | answer_rx_state()<br/>-> PrbWRetry | answer_rx_state()<br/>-> SettledNoRsv | answer_rx_state_reserved()<br/>-> SettledRsvOk |
| UnbindRefused | refuse_command()<br/>-> Unbound | refuse_command()<br/>-> PrbWAvail | refuse_command()<br/>-> PrbWDelay | refuse_command()<br/>-> PrbWResp | refuse_command()<br/>-> PrbWResp2 | refuse_command()<br/>-> PrbWRetry | refuse_command()<br/>-> SettledNoRsv | refuse_command()<br/>-> SettledRsvOk |
| Unbind | answer_unbound()<br/>-> Unbound | unbind()<br/>-> Unbound | unbind()<br/>-> Unbound | unbind()<br/>-> Unbound | unbind()<br/>-> Unbound | unbind()<br/>-> Unbound | unbind()<br/>-> Unbound | unbind()<br/>-> Unbound |
| ProbeAccepted | -x- | -x- | -x- | settle()<br/>-> SettledNoRsv | settle()<br/>-> SettledNoRsv | -x- | -x- | -x- |
| ProbeRejected | -x- | -x- | -x- | note_probe_rejected()<br/>-> PrbWRetry | note_probe_rejected()<br/>-> PrbWRetry | -x- | -x- | -x- |
| TmrNoResp | -x- | -x- | -x- | resend_probe()<br/>-> PrbWResp2 | note_probe_timeout()<br/>-> PrbWRetry | -x- | -x- | -x- |
| TmrRetry | -x- | -x- | -x- | -x- | -x- | delay_again()<br/>-> PrbWDelay | -x- | -x- |
| TmrRetryNoTalker | -x- | -x- | -x- | -x- | -x- | wait_for_talker()<br/>-> PrbWAvail | -x- | -x- |
| TmrDelay | -x- | -x- | send_probe()<br/>-> PrbWResp | -x- | -x- | -x- | -x- | -x- |
| TmrNoTk | -x- | -x- | -x- | -x- | -x- | -x- | drop_srp_and_delay()<br/>-> PrbWDelay | -x- |
| TmrNoTkNoTalker | -x- | -x- | -x- | -x- | -x- | -x- | drop_srp_and_wait()<br/>-> PrbWAvail | -x- |
| TalkerDiscovered | -x- | start_delay()<br/>-> PrbWDelay | note_discovered()<br/>-> PrbWDelay | note_discovered()<br/>-> PrbWResp | note_discovered()<br/>-> PrbWResp2 | note_discovered()<br/>-> PrbWRetry | note_discovered()<br/>-> SettledNoRsv | note_discovered()<br/>-> SettledRsvOk |
| TalkerDeparted | -x- | -x- | wait_for_talker()<br/>-> PrbWAvail | wait_for_talker()<br/>-> PrbWAvail | wait_for_talker()<br/>-> PrbWAvail | wait_for_talker()<br/>-> PrbWAvail | note_departed()<br/>-> SettledNoRsv | note_departed()<br/>-> SettledRsvOk |
| TalkerRegistered | -x- | -x- | -x- | -x- | -x- | -x- | note_registered()<br/>-> SettledRsvOk | -x- |
| TalkerUnregistered | -x- | -x- | -x- | -x- | -x- | -x- | -x- | drop_srp_and_delay()<br/>-> PrbWDelay |
| TalkerUnregisteredNoTalker | -x- | -x- | -x- | -x- | -x- | -x- | -x- | drop_srp_and_wait()<br/>-> PrbWAvail |
