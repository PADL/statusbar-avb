<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->

# AVB entity persistent state

`avb_entity_persistent_state.hpp` keeps what controllers set on an AVB entity
across a restart, in one TOML file the kit owns. Milan v1.2 requires these
settings to be saved in non-volatile memory.

## What is kept

- user names (SET_NAME)
- stream formats, sampling rates and clock sources
- the talkers' presentation time offsets
- the stream ports' dynamic audio mappings that differ from the entity's default
- the listener's bindings: classic fast-connect goals and Milan bound state
  (the file absorbs the older `avb_entity_listener_bindings.hpp` file)
- the system unique id and the Media Clock Reference user settings

Not kept (Milan v1.2 Section 5.3.4): the lock and the registered controllers.

A listener binding records the talker a stream input connects to and the
input's stream format when it was saved (0: unknown, e.g. imported); a binding
is not restored onto an input whose format differs. A Milan input also keeps
its binding parameters (Milan v1.2 Section 5.3.8.3): the controller that bound
it and whether it is started (Section 5.3.8.7).

## Format (version 1)

Unknown keys are ignored, so a later version can add keys an older reader
skips (the Milan bound state in `[[listener_binding]]` was added this way).
u64 values are hex strings.

```toml
version = 1
entity_model_id = "0x001b92fffe000001"
configured_entity_name = "Bridge"              # the name the entity was given
configured_presentation_offset_ns = 2000000    # the offsets it started at
system_unique_id = 1234                        # absent: 0, the default

[[name]]                  # descriptor_type, descriptor_index, name_index
descriptor_type = 0
descriptor_index = 0
name_index = 0
value = "FOH bridge"

[[stream_format]]
descriptor_type = 5       # STREAM_INPUT, or 6 STREAM_OUTPUT
descriptor_index = 0
format = "0x0205022002006000"

[[sampling_rate]]
audio_unit = 0
rate = 96000

[[clock_source]]
clock_domain = 0
clock_source = 1

[[presentation_offset]]
stream_output = 0
ns = 2000000

[[listener_binding]]
stream_input = 0
talker_entity_id = "0x001b92fffe000002"
talker_unique_id = 0
format = "0x0205022002006000"                  # the input's format (optional)
controller_entity_id = "0x001b92fffe000003"    # Milan: who bound it (optional)
started = false                                # Milan: stopped (optional, default true)

[[media_clock_reference]]
clock_domain = 0
user_priority = 200
domain_name = "Stage"

[[audio_map]]             # a stream port's dynamic mappings
descriptor_type = 14      # STREAM_PORT_INPUT
descriptor_index = 0
# [stream_index, stream_channel, cluster_offset, cluster_channel] each
mappings = [[0, 1, 0, 0], [0, 0, 1, 0]]
```

## Loading

The file is keyed by the entity model id: a file written for another model is
discarded (Milan v1.2 Section 5.5.1.4: the state belongs to the model).

- A missing file, when a legacy `avb_entity_listener_bindings` path is given,
  imports that file's bindings once; the caller then writes the new file and
  the old file is not written again.
- A corrupt file is moved to `<path>.bad` and the entity starts fresh.
- A file that cannot be read, or is of a later version, is left as it is, and
  the caller must not write the file that run: it would destroy what it could
  not read.

## Writing

The file is written whole and atomically (a temporary file in the same
directory, fsync, rename), from the reactor thread only, never the media
thread.

## See also

- [`AVB_ENTITY_KIT.md`](AVB_ENTITY_KIT.md) - what the kit restores at start
- [`MILAN_LISTENER.md`](MILAN_LISTENER.md) - the saved listener bindings
