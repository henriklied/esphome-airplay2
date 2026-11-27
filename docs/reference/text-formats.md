# Text formats

Two settings can be written as text, so a Home Assistant `text` entity can change them without a
reflash: the EQ filter cascade and the pair or group role.

## EQ filters

Used by `set_dsp_filters_spec()` and returned by `get_dsp_filters_spec()`. See
[Tuning from Home Assistant](../configuration/runtime-tuning.md).

```text
spec     = "flat" | section *7( ";" section )
section  = type "," frequency_hz "," q "," gain_db
type     = "pk" | "ls" | "hs" | "hp" | "lp" | "no"
```

| Code | Filter |
|---|---|
| `pk` | peaking |
| `ls` | low shelf |
| `hs` | high shelf |
| `hp` | high-pass |
| `lp` | low-pass |
| `no` | notch |

| Field | Range |
|---|---|
| `frequency_hz` | 10 to 24000 |
| `q` | 0.05 to 20 |
| `gain_db` | -24 to 24 |

Rules:

- All four fields are required in every section. There are no defaults.
- At most 8 sections.
- Type codes are lowercase. Spaces are not part of the format: a space around a type code or after
  a number makes the text invalid.
- A trailing `;` is invalid.
- `gain_db` is accepted on `hp`, `lp` and `no` but ignored. (YAML rejects a `gain:` on those types.)
- `flat` is an explicit empty cascade.
- An empty string means "not set". `set_dsp_filters_spec("")` returns `true` and changes nothing.
- If any part is invalid, the whole text is refused, the call returns `false`, and the current
  cascade keeps playing.

The range check does not know the sample rate. A section above 0.49 times the output rate (21609 Hz
at 44.1 kHz) is accepted, then skipped at runtime with a warning in the log.

`get_dsp_filters_spec()` writes frequency and gain in shortest form and Q with two decimals, so a Q
of `0.7071` reads back as `0.71`.

Examples:

```text
hp,80,0.71,0;ls,150,0.70,1.5;pk,200,0.90,-2
hp,80,0.70,0;ls,150,0.70,4;hs,6000,0.70,-2
flat
```

The second line is the cascade from `examples/dsp-speaker-correction.yaml`. Eight sections fit in a
255-character `text` entity.

## Group role

Used by `set_group_spec()` and returned by `get_group_spec()`. See
[Stereo pair and groups](../configuration/stereo-pair.md#change-the-role-at-runtime).

```text
spec   = "standalone" | "yaml" | "" | pair *( ";" pair )
pair   = key "=" value
```

| Key | Values | Default |
|---|---|---|
| `role` | `standalone`, `primary`, `secondary`. Required. | |
| `peer` | Dotted IPv4. Required for a primary. For a secondary, the only primary accepted; empty accepts any. | empty |
| `port` | 1 to 65534 | `7011` |
| `channel` | `left`, `right`, `mono`, `stereo`: what a primary forwards | `right` |
| `clock` | `auto`, `ptp`, `relay`: what a secondary plays against | `auto` |
| `delay` | Secondary playout delay in microseconds, 0 to 100000 (as `delay:` in YAML) | `0` |
| `name` | A primary's pair destination, up to 63 bytes, no `;` | empty |
| `standalone` | `0` or `1`: a secondary keeps its own destination | `0` |

Rules:

- The whole text is at most 200 characters.
- Keys can come in any order. Missing keys take the defaults above, which match the YAML schema.
- An unknown key, an unknown value or a malformed IPv4 address makes the text invalid. Nothing is
  stored and the board does not reboot.
- A key that does not apply to the role, such as `channel` on a secondary, is accepted and dropped.
  (YAML rejects it.)
- `standalone` on its own, or `role=standalone`, is a board with no pair role.
- `yaml`, or an empty string, forgets the stored role so the board uses `pair:` from YAML again.

`get_group_spec()` writes only what the role uses: `channel` and `name` for a primary, `clock`,
`delay` and `standalone` for a secondary, and `port` only when it is not 7011.

Examples:

```text
standalone
role=primary;peer=10.0.40.146;channel=stereo;name=Kitchen
role=secondary;peer=10.0.40.90;clock=auto;delay=0;standalone=1
yaml
```
