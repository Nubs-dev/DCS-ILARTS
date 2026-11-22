# DCS ILARTS console (`<ilarts-console>`)

The ILARTS station interface as one framework-free web component. The same file runs in two hosts:

- **The desktop application (`ILARTS.exe`).** It shows `index.html` in a WebView2 composed over its Direct3D pictures (`viewer/console.inc`).
- **A web page.** It shows the pictures from streams.

| File | What it is |
|---|---|
| `ilarts-console.js` | The component. No dependencies and no build step; load it as a plain script or a module. |
| `index.html` | The desktop host page: the bridge to `ILARTS.exe`. |
| `fonts/` | Saira and IBM Plex Mono (SIL Open Font License), with `fonts.css`. |
| `demo.html`, `demo-img/` | A browser demo with stand-in pictures. Query parameters: `?fit` sizes the monitors to the window; `?native` shows the desktop arrangement, with the screens cut open over a test pattern. |

## Using it

```html
<link rel="stylesheet" href="fonts/fonts.css">   <!-- @font-face must be in the document, not the shadow root -->
<script src="ilarts-console.js"></script>
<ilarts-console fit style="height:100vh"></ilarts-console>
<script>
  const el = document.querySelector('ilarts-console');
  el.state = { /* see State */ };
  el.addEventListener('command', e => handle(e.detail));     // see Commands
  el.setVideo('program', videoElement);                     // see Pictures
</script>
```

The element owns no application state. The host sets `el.state` whenever something changes, and the element redraws from it. For immediate feedback, knobs show a new value at once, and the host's next state confirms it.

### Attributes

| Attribute | Effect |
|---|---|
| `fit` | The element takes its container's height, and a layout solver places everything to fill it: the monitors with their channel strips, the previews (layout 1 + 4) and the rack of shared modules, which always goes under the monitors. In small windows it scales the controls down (to 55 %) rather than shrink the pictures; when even that does not fit, everything stacks at full width and the page scrolls. |
| `native-video` | Desktop only: the screens are cut out of the page and the host draws the pictures behind them. It reports their positions with the `layout` command. |

## Pictures (web hosts)

```js
el.setVideo(id, element)   // id: 'program' (monitor A), 'second' (monitor B), 'p0' .. 'p3' (the 1 + 4 previews)
```

`id` is `program`, `second`, or `p0` to `p3` for the four previews of layout 1 + 4 (a host that sends no `previews` gets the old previews: every source not on A, by source id). `element` is any element: `<video>`, `<img>` or `<canvas>`. It fills the monitor (`object-fit: cover`). Pass `null` to clear it.

In a web host, picture adjustments and the CCTV look are approximated with CSS on monitors A and B (greyscale, brightness, contrast, scan lines and vignette). Setting `nativeLook: true` in the state turns that off.

### Streams from the desktop application

- **Up to four streams per desktop.** Each one (`[stream]`, `[stream2]` .. `[stream4]` in `ILARTS.ini`: `url`, `key`, `source`, `tiles`) sends monitor A, monitor B, one source, or a composite of up to four in one 1920 x 1080 picture.
- **Browsers cannot play RTMP.** A media server must repackage the stream. For example, MediaMTX:
  1. Takes the desktop's RTMP: `[stream] url=rtmp://server:1935/ilarts`, `key=<pilot or seat>`.
  2. Serves it as WebRTC (WHEP, under a second of delay) or as HLS (a few seconds; works everywhere).
- **The web console only views.** Its commands have no way back to the desktop application. A web host should therefore:
  - ignore the commands, or use them only for its own player, such as `source` to choose which pilot's stream fills the program monitor;
  - send `windowButtons: false`;
  - mark controls it does not support as unavailable (`picture.unavailable`, `vtr.ok: false`).

Example with HLS:

```js
const v = Object.assign(document.createElement('video'), { muted: true, autoplay: true, playsInline: true });
if (v.canPlayType('application/vnd.apple.mpegurl')) v.src = url; else { const h = new Hls(); h.loadSource(url); h.attachMedia(v); }
el.setVideo('program', v);
```

## State

All fields are optional. Missing objects show as empty or off.

| Field | Type | Meaning |
|---|---|---|
| `status` | string | Title-bar readout, e.g. `ISLAND · 768×768 · 102 FPS`. |
| `flash` | string | A short confirmation shown in place of the presets hint. |
| `windowButtons` | bool | `false` hides minimise, maximise and close (web hosts). |
| `nativeLook` | bool | `true`: the host renders the adjustments and the CCTV look itself, so there are no CSS stand-ins. |
| `settings` | bool | `false` makes SETTINGS FILE unavailable. |
| `guide` | bool | `false` hides the title bar's ? key (the user guide). |
| `update` | object or `null` | The title bar's update indicator (a download icon and `label`), shown only with an update: `label` (`'1.1.0 AVAILABLE'`, `'1.1.0 · 37%'`, `'UPDATE FAILED'`), `tone` (`'green'`, `'amber'`), `tip`, `version`, and `menu`: `[{id, label, on}]`, the items its click offers (`on` false: shown greyed). |
| `bench` | object or `null` | The title bar's benchmark lamp, shown only with a state: `state` (`'armed'`, `'running'`, `'complete'`, `'error'`) and `tip` (its tooltip). |
| `knobs` | `{exp, bri, con, gam}` | Each `{min, max, step, log}` overrides a knob's range. `log: true` gives a logarithmic sweep. The defaults are those of the hand-off spec. |
| `layout` | string | The monitor wall: `'1'` (monitor A), `'2'` (A and B, each with its own channel strip) or `'q'` (A and four previews). Default `'q'`. |
| `monitors[]` | array | Monitor A (the program: recorded on the main channel, streamed, replayed) and monitor B. Each: `id` (the source shown), `name`, `plate`, `res`, `signal`, `rec` (the VTR records this source). Burn-ins: `burnIns` (false hides time, ship and name), `time`, `ship`, `burnName`. `message` replaces "NO SIGNAL" and may span lines. `crosshair` applies to web hosts only. `picture` (or `null`: the source has no levels, like LSO 2): `group` (whose levels the knobs set; monitors showing the same group share them), `auto`, `autoRange`, `exp`, `bri`, `con`, `gam`, `unavailable` `{auto, reset, exp, bri, con, gam}`; for web hosts `adj` (false: no CSS stand-in), `cctv`, `str` (0-200). `ptz` (or `null`: not a PTZ camera): `id`, `name`, `live`, `manual` (under the joystick), `track` (false: the camera follows nothing, as on a hangar deck position: no AUTO or TARGET key), `armed` (AUTO on), `auto` (armed and not under the joystick: the AUTO lamp), `target` and `targets` (`[[id, label]]`: what AUTO follows), `pan`, `tilt` (degrees off the rest view), `zoom` (factor); `tune` (only with the host's camera tuning on: the TUNE key and its panel): `name` (the camera's or position's entry name), `known` (false: values not known yet, keys off), `tuned` (the host has an entry for it: RESET on), `pos`, `look` (`[x, y, z]`, m), `fov` (deg), `step` and `steps` (m), `alert` (a write that failed). `views` (a camera with positions to choose from, e.g. the deck camera): the names, `hangar` (the indices of the positions on the hangar deck: the lists show them under HANGAR DECK, the others under FLIGHT DECK) and `view`, the index chosen; with them `cycle` (the monitor's deck cycle, its CYCLE key): `on`, `seconds` (the interval) and `sel` (the indices into `views` it steps through). |
| `program`, `picture` | object | Older hosts: without `monitors`, monitor A is made of these (the `monitors[]` fields, `picture` apart). |
| `previews` | array | Layout 1 + 4: the four previews' source ids (`''` = none). Each preview has a source selector; clicking its picture swaps it with monitor A. Without it, the previews show the sources not on A. |
| `sources[]` | array | `id`, `name` (button), `key`, `plate` (engraved label), `signal`, `selected` (program tally), `rec` (its REC switch: the VTR records it), `fps`. `group` (e.g. `'deck'`; a deck position also has `hangar`, true on the hangar deck): the sources of a group share one source key and one REC key (`id` = the group); a monitor showing one of them steps through them with its view selector (`monitors[].views`). |
| `ships[]` | array | `id`, `unit`, `hull`, `name` (the ship's name, e.g. `'ROOSEVELT'`; shown under the hull, else `unit`), `on`. |
| `presets[]` | array | `name`, `key`, `saved`, `fixed` (cannot be saved, renamed or cleared), `active` (every picture it holds looks as it says). A preset holds the levels of every picture: the PLAT and each camera. |
| `cameras[]` | array | `id`, `name`, `on` (its power rocker). `live`: green LED. `starting`: blinking amber. |
| `settingsForm` | object | Present while the settings dialog is open. `rev` (re-read the values when it changes), `file`, `message`, `restart` (show RESTART NOW). `sections[]` has `title`, `tip` (optional: an **i** beside the panel's name with this tooltip) and `fields[]`. Each field has `key` (`section.key`), `label`, `type` (`text`, `password`, `number`, `bool`, `select`, `path`), `value`, `min`, `max` (for `text`: its longest), `step`, `unit`, `hint`, `restart`, `default` (a stream target's fields: what CLEAR sets), and for `select` `options` (`[[value, label]]`). |
| `vtr` | object | `ok` (false: no recorder), `counter`, `line1`, `line2`, `mode` (`'auto'` or `'manual'`), `autoFilter` (whose approaches AUTO records: `'own'`, `'section'`, `'flight'` or `'all'`; absent: no ACFT key), `noAcft` (true: the player has flown no aircraft this mission, so only `'all'` records), `composite` (COMP: recordings and stills are one composite picture of the REC sources), `rec`, `play`, `replayTime`, `speed` (e.g. `'1/2x'`), `canSave`, `folder` (false: FOLDER unavailable), `folderInfo` (the recordings folder, e.g. `'12 CAPTURES · 3.4 GB'`; absent: not shown). |
| `bb` | object | `on`, `text`, `alert` (amber text). |
| `stream` | object | `on` (any stream). `state`: `'off'`, `'connecting'`, `'live'` or `'retrying'`, over all streams on. `text`: the status while on, a line per stream. `targets[]`: per stream `set` (has a server), `state` and `label` (up to 6 characters, shown on its key instead of the number). |

The element keeps only the settings dialog's unsaved edits. Each monitor's joystick drives the camera that monitor shows.

## Commands

The `command` event's `detail` is one of these:

| Command | Sent when |
|---|---|
| `{cmd:'source', id, mon}` | A source key on a monitor's strip (`mon` 0 = A, 1 = B); `id` may be a group (`'deck'`: the host picks the position). |
| `{cmd:'view', mon, dir}`, `{cmd:'view', mon, index}` | Monitor `mon`'s deck position: ◀ / ▶ (`dir` −1 / 1; skips those other monitors show) or picked from the list. |
| `{cmd:'cycle', mon, on}`, `{cmd:'cycle', mon, sel}`, `{cmd:'cycle', mon, seconds}` | Monitor `mon`'s deck cycle: CYCLE on / off; the positions ticked in its pop-up (`sel`: indices into `views`, comma-separated, e.g. `'0,3,5'`); the interval (2 to 3600 s). |
| `{cmd:'preview', slot, id}` | A preview's source (`''` = none). |
| `{cmd:'previewTake', slot}` | A preview's picture is clicked: its source goes to monitor A, A's to the preview. |
| `{cmd:'wall', mode}` | A MONITORS key: `mode` is `'1'`, `'2'` or `'q'`. |
| `{cmd:'swap'}` | A ⇄ B: the two monitors change pictures. |
| `{cmd:'recArm', id}` | A REC key: on a monitor's strip (the source it shows) or in the VTR's REC row. |
| `{cmd:'ship', id}`, `{cmd:'shipStep', dir}` | A ship is picked from the list, or stepped with ◀ / ▶. |
| `{cmd:'camera', id}` | A camera's power rocker is flipped. |
| `{cmd:'ptz', cam, pan, tilt, zoom}` | The joystick: rates −1..1, sent 10 times a second while held, then once as zeros. Pan + is right, tilt + is up, zoom + is in. |
| `{cmd:'ptzRest', cam}` | REST: back to the rest view, automatic tracking off. |
| `{cmd:'ptzAuto', cam}` | AUTO: back to the rest view, then follow the target. |
| `{cmd:'ptzTarget', cam, target}` | What AUTO follows (an id of `ptz.targets`). |
| `{cmd:'tune', cam, field, axis, dir}` | Camera tuning: `field` `pos`, `look` (axis 0-2, by one step) or `fov` (by one degree), `dir` −1 / 1. |
| `{cmd:'tuneStep', index}` | Camera tuning: the step, an index into `tune.steps`. |
| `{cmd:'tuneReset', cam}`, `{cmd:'tuneCopy', cam}` | Camera tuning: RESET (drop the entry), COPY (its Lua line to the clipboard). |
| `{cmd:'knob', key, value, mon}` | A knob on monitor `mon`'s strip is dragged, scrolled, or double-clicked (back to its default: exposure and brightness 0, contrast and gamma 1) (`key`: `exp`, `bri`, `con`, `gam`). |
| `{cmd:'auto', mon}`, `{cmd:'reset', mon}` | AUTO or RESET on monitor `mon`'s strip. |
| `{cmd:'preset', index, save}` | A preset is clicked; `save` is true with Ctrl or Shift held. |
| `{cmd:'rename', index, name}`, `{cmd:'presetClear', index}` | The preset's right-click menu (it also saves, with `{cmd:'preset', index, save: true}`). `name` empty: the preset's number. |
| `{cmd:'keys', page}` | A pop-up or dialog opened (`page` true) or closed: while one is open the page keeps the keyboard. |
| `{cmd:'autoFilter', value}` | The ACFT key: `value` is the next of `'own'`, `'section'`, `'flight'`, `'all'`. |
| `{cmd:'mode', auto}`, `'comp'`, `'rec'`, `'play'`, `'stop'`, `'save'`, `'folder'` | A VTR button is pressed. The REPLAY preview sends `play`. |
| `{cmd:'bb'}`, `'bbSave'`, `{cmd:'stream', on}`, `{cmd:'stream', n, on}` | Black box and stream rockers (every stream with a server), one stream (n 0-3), and SAVE. |
| `{cmd:'settings'}`, `'settingsClose'`, `{cmd:'settingsSave', values}`, `{cmd:'settingsAction', id, key}` | The settings dialog. `values` maps each field key to its value. `id` is `browse` (with `key`), `openFile`, `export`, `import` or `restart`. |
| `{cmd:'win', what}`, `{cmd:'drag'}` | Title bar: `what` is `min`, `max` or `close`. |
| `{cmd:'update', what}` | Title bar: an item of the update indicator's menu (`what` = its `id`: `'install'`, `'notes'`, `'skip'`, `'cancel'`). |
| `{cmd:'guide'}`, `{cmd:'benchLamp'}` | Title bar: the ? key; the benchmark lamp clicked after a run (complete or error). Armed or running, the lamp opens the settings dialog at the Benchmark panel instead (`{cmd:'settings'}`). |
| `{cmd:'layout', rects, dpr}` | `native-video` only: the screens' rectangles (`[x, y, w, h]` in CSS pixels of the viewport), plus `titlebar` and `winbtns`. |

The desktop bridge (`index.html`) adds these messages:

- page to application: `{cmd:'ready'}` and `{cmd:'key', vk}`;
- application to page: `{field:{key, value}}` (passed to `el.setSettingsField`).
