# Milan Listener Discovery

![diagram](milan_adp_discovery_sm.svg)


**Implements:** Milan v1.3 Section 5.6.4

| Event | Stopped | TkNotDiscovered | TkDiscovered |
|-------|--------|--------|--------|
| Start | start()<br/>-> TkNotDiscovered | start()<br/>-> TkNotDiscovered | start()<br/>-> TkNotDiscovered |
| Stop | stop()<br/>-> Stopped | stop()<br/>-> Stopped | stop()<br/>-> Stopped |
| Available | -x- | discover()<br/>-> TkDiscovered | -x- |
| Refreshed | -x- | -x- | refresh()<br/>-> TkDiscovered |
| Restarted | -x- | -x- | rediscover()<br/>-> TkDiscovered |
| RestartedElsewhere | -x- | -x- | depart()<br/>-> TkNotDiscovered |
| Departing | -x- | -x- | depart()<br/>-> TkNotDiscovered |
| TmrNoAdp | -x- | -x- | depart()<br/>-> TkNotDiscovered |
