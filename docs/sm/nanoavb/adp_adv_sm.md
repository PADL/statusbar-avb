# ADP Advertise

![diagram](adp_adv_sm.svg)


**Implements:** IEEE 1722.1-2021 Clause 6.2.5 (ADP Advertise / entity-side)

| State | Entry | Exit |
|-------|-------|------|
| Off | stop_adp() | - |

| Event | Start | Off | Advertising |
|-------|--------|--------|--------|
| UCT | init()<br/>-> Off | -x- | -x- |
| Enable | -x- | start_adp()<br/>-> Advertising | -x- |
| Disable | -x- | -x- | -> Off |
| Tick | -x- | -x- | maybe_announce()<br/>-> Advertising |
| Error | -x- | -x- | -> Off |
