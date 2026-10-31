# ADP Advertise

![diagram](adp_adv_sm.svg)


**Implements:** Milan v1.2 Section 5.6.3; IEEE 1722.1-2021 Clause 6.2.4 (ADP Advertise)

| Event | Start | Down | Waiting | Delay |
|-------|--------|--------|--------|--------|
| LinkUp | start_startup_delay()<br/>-> Delay | start_delay()<br/>-> Delay | -x- | -x- |
| LinkDown | -> Down | -x- | stop_timers()<br/>-> Down | stop_timers()<br/>-> Down |
| RcvDiscover | -x- | -x- | start_delay()<br/>-> Delay | -x- |
| TmrAdvertise | -x- | -x- | start_delay()<br/>-> Delay | -x- |
| TmrDelay | -x- | -x- | -x- | advertise()<br/>-> Waiting |
| GmChange | -x- | -x- | start_delay()<br/>-> Delay | -x- |
| EntityChange | -x- | -x- | start_delay()<br/>-> Delay | -x- |
| Shutdown | -x- | -x- | depart()<br/>-> Start | depart()<br/>-> Start |
