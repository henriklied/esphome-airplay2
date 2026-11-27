# Tuning from Home Assistant

Every DSP parameter can be changed from a lambda, so a Home Assistant entity can drive it and you tune
by ear in the room instead of spending an OTA cycle per change.

There are two ways to change the filter cascade. Pick one per board:

- **Positional setters**, one `number` entity per parameter. Type and count of filters stay as
  in YAML.
- **Text form**, one `text` entity holding the whole cascade. Type and count can change, and the
  value survives reboots.

!!! note

    The setters recompute coefficients and must run on the main loop. A YAML lambda runs there, so
    calling them from `on_value`, `turn_on_action` and similar is correct.

## Positional setters

```cpp
id(airplay).set_dsp_filter_frequency(0, 90.0f);
id(airplay).set_dsp_filter_gain(1, 3.0f);
id(airplay).set_dsp_filter_q(1, 0.8f);
id(airplay).set_dsp_preamp(-3.0f);
id(airplay).set_dsp_enabled(false);
```

The index is positional into `dsp: filters:`. Reordering that list repoints every caller. An index
outside the list is ignored, with a warning in the log.

Wire each one to a `number`. Apply the value from `on_value`, not `set_action`: `restore_value`
republishes the stored value at boot without going through the set action, and `on_value` covers
both the restore and a user edit.

```yaml
number:
  - platform: template
    name: "DSP bass lift"
    id: dsp_low_shelf_gain
    unit_of_measurement: dB
    entity_category: config
    optimistic: true
    restore_value: true
    initial_value: 4
    min_value: -6
    max_value: 9
    step: 0.5
    mode: slider
    on_value:
      then:
        - lambda: id(airplay).set_dsp_filter_gain(1, x);
```

`examples/dsp-speaker-correction.yaml` wires a high-pass frequency, two shelf gains, the preamp and a
bypass switch this way.

## Dynamic stage setters

| Setter | Stage |
|---|---|
| `set_dsp_loudness_boost(db)` | Loudness `max_boost` |
| `set_dsp_bass_enhancer_amount(x)` | Bass enhancer `amount` |
| `set_dsp_stereo_width_amount(x)` | Stereo width `width` |
| `set_dsp_centre_lock(x)` | Stereo width `centre_lock` |
| `set_dsp_ambience_amount(x)` | Ambience `amount` |
| `set_dsp_ambience_decay(ms)` | Ambience `decay` |
| `set_dsp_crosstalk_amount(x)` | Crosstalk cancel `amount` |
| `set_dsp_crosstalk_delay(us)` | Crosstalk cancel `delay` |
| `set_dsp_bass_protection(hz)` | Bass protection `frequency`, `0` turns it off |
| `set_dsp_bass_limiter_threshold(db)` | Bass limiter `threshold` |
| `set_dsp_limiter_enabled(bool)` | Limiter on or off |
| `set_dsp_limiter_threshold(db)` | Limiter `threshold` |

Each keeps the stage's other fields. Setting an amount to `0` (or width to `1.0`) turns the stage
off. Three getters read back what runs:

- `get_dsp_loudness_now()`: the loudness lift in effect, in dB.
- `take_dsp_limiter_reduction()`: the deepest limiter gain reduction since the last call, in dB.
- `take_dsp_bass_limiter_reduction()`: the same for the bass band.

## Text form

`set_dsp_filters_spec(text)` replaces the whole cascade. `get_dsp_filters_spec()` returns what
runs, in the same form. One section per `;`, fields `type,frequency_hz,q,gain_db`:

```text
hp,80,0.71,0;ls,150,0.70,1.5;pk,200,0.90,-2
```

Types are `pk ls hs hp lp no`. `flat` is an empty cascade. An empty string changes nothing, so after boot the YAML filters keep
playing.
Text that does not parse is refused and nothing changes. Full grammar and limits:
[Text formats](../reference/text-formats.md).

Eight sections fit in a 255-character `text` entity, which also gives you restore across reboots:

```yaml
text:
  - platform: template
    name: "EQ filters"
    id: dsp_filters
    mode: text
    max_length: 255
    optimistic: true
    restore_value: true
    initial_value: ""          # empty: keep the YAML filters
    on_value:
      then:
        - lambda: |-
            if (!id(airplay).set_dsp_filters_spec(x)) {   // refused: show what runs
              id(dsp_filters).publish_state(id(airplay).get_dsp_filters_spec());
            }
esphome:
  on_boot:
    priority: -100             # after setup(), so the restored text wins over YAML
    then:
      - lambda: |-
          if (id(dsp_filters).state.empty() ||
              !id(airplay).set_dsp_filters_spec(id(dsp_filters).state)) {
            id(dsp_filters).publish_state(id(airplay).get_dsp_filters_spec());
          }
```

On boot this applies the restored text, or, if there is none, publishes the YAML cascade so the
entity shows what plays.

!!! warning

    Do not mix the two styles on one board. The positional setters address filters by index, and a
    text edit can reorder or remove them.

## Switching an existing board to the text form

If your Home Assistant sliders call the positional setters with `restore_value: true`, the values you
tuned live only in those entities. When the reflashed board stops reporting them, Home Assistant
removes the entities and the values are gone. The board then plays the `dsp: filters:` block from YAML
until the text is set.

Before you reflash:

1. Read the current values off the Home Assistant sliders, or from the cascade the component logs
   at boot.
2. Write them into the `dsp: filters:` block, in the same order.
3. Remove the slider entities from your YAML, add the `text` entity above, and flash.

The text starts empty, so the board keeps playing the YAML filters, now with your tuned values.

## Related

- What each filter and stage does: [Output DSP](output-dsp.md), [Dynamic stages](dynamic-stages.md).
- A full tuning session: [Correct a speaker by ear](../guides/speaker-correction.md).
