// DCS ILARTS console: the station interface as one custom element, <ilarts-console>.
//
// The element owns no application state. Its host gives it a state object and listens for commands:
//
//   const el = document.querySelector('ilarts-console');
//   el.state = { ... };                                   // see ui/README.md for the fields
//   el.addEventListener('command', e => host(e.detail));  // { cmd: 'source', id: 'island', mon: 0 } ...
//
// Two hosts use it:
//   * the desktop application (ILARTS.exe): the page runs in a WebView2 laid over the Direct3D pictures. The
//     monitor screens are transparent and the application draws the live pictures behind them, at the
//     rectangles the element reports ({ cmd: 'layout', rects }). Attribute `native-video`.
//   * a web page: the host puts a <video> (or any element) into a monitor with
//     el.setVideo(id, element); picture adjustments and the CCTV look are then approximated with CSS.
// Attribute `fit`: the element takes its container's height (give it one, e.g. 100vh).
//
// Layout. Monitors with their own channel strip under them (source keys, REC, levels, PTZ joystick: every control
// sits under the picture it acts on), and one rack of shared modules (monitor layout, ship, presets, cameras, VTR,
// black box, stream). The wall shows one monitor, two side by side or stacked, or one with four small previews
// (1 + 4). A solver places it all to fill the window whatever its shape - 16:9, ultrawide, half a screen; the rack
// always goes under the monitors. No framework, no build step.
(function () {
  // lamps in the keycaps (lit, dark) and panel LEDs
  const LAMP = { red: ['#FF4A3D', '#5A2A27'], green: ['#62E06B', '#29452B'], amber: ['#FFB547', '#5A4520'], white: ['#F6F8FF', '#55595F'] };
  const LED = { red: ['#E0524E', '#3A2222'], green: ['#5FD068', '#1E2E20'], amber: ['#E7B04F', '#2E2716'] };
  // knob: label, min, max, step, format, wheel steps per notch. A host can override min / max / step and ask
  // for a logarithmic sweep with state.knobs = { con: { min: 0.25, max: 16, log: true } }.
  const KNOBS = {
    exp: ['Exposure', -3, 3, 0.05, x => (x >= 0 ? '+' : '') + x.toFixed(2) + ' EV', 2],
    bri: ['Brightness', -1, 1, 0.01, x => (x >= 0 ? '+' : '') + x.toFixed(2), 2],
    con: ['Contrast', 0.5, 2, 0.01, x => '×' + x.toFixed(2), 2],
    gam: ['Gamma', 0.5, 2, 0.01, x => x.toFixed(2), 2],
  };
  const TICKS = Array.from({ length: 11 }, (_, i) => {
    const a = (-135 + i * 27) * Math.PI / 180, s = Math.sin(a), c = -Math.cos(a);
    return `<line x1="${(24 * s).toFixed(1)}" y1="${(24 * c).toFixed(1)}" x2="${(27 * s).toFixed(1)}" y2="${(27 * c).toFixed(1)}"></line>`;
  }).join('');
  // Geometry the CSS and the layout solver share (px).
  const M = 10;            // margin around the wall and the rack
  const GAP = 10;          // between monitors, previews and the rack
  const UB = 11;           // monitor unit: border 1 + padding 10
  const BZ = 9;            // bezel: border 1 + padding 8
  const PL = 24;           // name plate under a monitor screen (margin 6 + 18)
  const SG = 8;            // bezel to channel strip
  const SCG = 8, SRG = 8;  // channel strip: gaps between its groups
  const TB = 7;            // preview tile: border 1 + padding 6
  const TP = 22;           // preview plate (margin 6 + 16)
  const RG = 8;            // rack: gap between modules
  const MONS = ['program', 'second'];  // the monitors' screen ids (rects, setVideo)

  const CSS = `
:host{display:block;position:relative;background:#0B0D0F;color:#E6EDF3;font-family:'Saira','Segoe UI',system-ui,sans-serif;user-select:none;-webkit-user-select:none}
*{box-sizing:border-box;scrollbar-width:thin;scrollbar-color:#4A525A #15181B}
.mono,.eng,.plate,.lcd,.kc,.burn,.fld,.hint{font-family:'IBM Plex Mono',Consolas,'Cascadia Mono',monospace}
.eng{font-size:9.5px;font-weight:600;letter-spacing:.14em;text-transform:uppercase;color:#AEB6BE}
.dim{color:#7D8690}
.plate{display:inline-block;font-size:9.5px;font-weight:600;letter-spacing:.14em;text-transform:uppercase;color:#C9D0D6;background:#16191C;border:1px solid #444C54;border-radius:2px;padding:2px 7px;white-space:nowrap}
.pnl{position:relative;background:#2A2F35;border:1px solid #3A4148;border-radius:4px;box-shadow:inset 0 1px 0 rgba(255,255,255,.05),inset 0 -2px 0 rgba(0,0,0,.25);padding:11px 12px 10px;display:flex;flex-direction:column;gap:8px}
.scr{position:absolute;width:6px;height:6px;border-radius:50%;background:#4A525A;box-shadow:inset 0 0 0 1.5px #1C2024}
.row{display:flex;align-items:center;gap:6px}
.col{display:flex;flex-direction:column;gap:6px}
.sp{flex:1}
.lcd{display:inline-block;font-size:12px;font-weight:600;color:#7FC77A;background:#0A100B;border:1px solid #2E3B30;border-radius:2px;padding:3px 7px;text-align:center;letter-spacing:.04em;white-space:nowrap}
.lcd.sm{font-size:10.5px;padding:2px 6px}
.led{display:inline-block;width:8px;height:8px;border-radius:50%;box-shadow:inset 0 0 0 1px rgba(0,0,0,.5);flex:none}
.hint{font-size:8.5px;font-weight:500;letter-spacing:.1em;text-transform:uppercase;color:#7D8690;line-height:1.45}
/* keycaps: light, with a lamp window that lights when the mode is active */
.kc[hidden]{display:none!important}
.kc{position:relative;width:52px;height:40px;padding:11px 3px 0;border-radius:3px;border:1px solid #0E1012;background:linear-gradient(#E8EAE5,#C6C9C2);color:#1E2226;font-size:10px;font-weight:600;letter-spacing:.06em;cursor:pointer;box-shadow:inset 0 -3px 0 rgba(0,0,0,.2),inset 0 1px 0 rgba(255,255,255,.75),0 1px 2px rgba(0,0,0,.55);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;flex:none}
.kc.arw{font-size:15px;padding-top:7px}
.kc.warn{background:linear-gradient(#F29447,#D2691F);color:#FFFFFF;text-shadow:0 1px 1px rgba(0,0,0,.35)}
.kc.warn .k{opacity:.8}
.kc.warn[aria-disabled=true]{background:linear-gradient(#B9906F,#9D7A5D);color:#E9DDD3;text-shadow:none}
.kc.mini{height:24px;padding-top:6px;font-size:8px;letter-spacing:.1em}
.kc.mini .lamp{top:3px;width:12px;height:3px;margin-left:-6px}
.kc.w{width:auto;min-width:52px;padding-left:9px;padding-right:9px}
.kc .lamp{position:absolute;left:50%;top:5px;width:16px;height:5px;margin-left:-8px;border-radius:1.5px;box-shadow:inset 0 0 0 1px rgba(0,0,0,.45)}
.kc .k{position:absolute;right:3px;top:1px;font-size:7.5px;font-weight:500;letter-spacing:0;opacity:.6}
.kc:active{transform:translateY(1px)}
.kc[aria-disabled=true]{cursor:default;color:#7C8186;background:linear-gradient(#AEB2AC,#979B94)}
.kc[aria-disabled=true]:active{transform:none}
/* rocker switches: dark when off, glowing when on */
.rk{position:relative;width:28px;height:44px;padding:0;border-radius:5px;background:#0B0C0E;border:1px solid #2C3035;box-shadow:inset 0 1px 3px rgba(0,0,0,.85),0 1px 0 rgba(255,255,255,.04);cursor:pointer;flex:none}
.rk .face{position:absolute;inset:3px;border-radius:3px;background:linear-gradient(180deg,#7A3A33 0,#5A2621 14%,#3A1714 46%,#1E0A09 54%,#2A100E 100%);box-shadow:inset 0 1px 0 rgba(255,190,180,.28),inset 0 -1px 0 rgba(0,0,0,.6)}
.rk .i{position:absolute;left:50%;top:7px;width:2px;height:8px;margin-left:-1px;border-radius:1px;background:#B07A73}
.rk .o{position:absolute;left:50%;bottom:6px;width:9px;height:9px;margin-left:-4.5px;border-radius:50%;border:1.5px solid #B07A73}
.rk[aria-pressed=true] .face{background:radial-gradient(circle,rgba(255,236,190,.38) .7px,transparent 1.1px) 0 0/3px 3px,linear-gradient(180deg,#B8440B 0,#E9640F 44%,#FFA236 56%,#FFC968 100%);box-shadow:0 0 12px 2px rgba(255,140,30,.5),inset 0 1px 0 rgba(120,30,0,.6)}
.rk[aria-pressed=true] .i{background:#FFF4DE}
.rk[aria-pressed=true] .o{border-color:#FFF4DE}
.rk[aria-disabled=true]{cursor:default;opacity:.45}
.kn{display:flex;flex-direction:column;align-items:center;gap:3px;flex:none;width:56px}
.kn svg{cursor:ns-resize;outline:none}
.kn.na{opacity:.38}
.kn.na svg{cursor:default}
.kn .lcd{font-size:10px;padding:1px 3px;min-width:54px}
.scrn{position:relative;overflow:hidden;background:#050607;border-radius:3px;aspect-ratio:4/3}
:host([native-video]){background:transparent}
.ovroot{position:absolute;inset:0;overflow:hidden;border-radius:3px}
.top{position:absolute;left:0;top:0;width:0;height:0}
.top .ovroot{inset:auto}
.ovroot[data-pick]{cursor:pointer}
.vid{position:absolute;inset:0}
.vid>*{position:absolute;inset:0;width:100%;height:100%;object-fit:cover}
.burn{position:absolute;font-weight:600;color:#F2F2F2;text-shadow:0 0 2px #000,0 0 1px #000,0 0 3px #000;letter-spacing:.06em;white-space:pre;pointer-events:none}
.nosig .msg{white-space:pre-line;text-align:center;max-width:86%;line-height:1.5}
.ovl{position:absolute;inset:0;pointer-events:none}
.bars{position:absolute;inset:0;background:repeating-linear-gradient(90deg,#BDBDBD 0 14.28%,#BDBD3C 14.28% 28.56%,#3CBDBD 28.56% 42.84%,#3CBD3C 42.84% 57.12%,#BD3CBD 57.12% 71.4%,#BD3C3C 71.4% 85.68%,#3C3CBD 85.68% 100%);filter:saturate(.35) brightness(.55)}
@keyframes blinkkf{0%,100%{opacity:1}50%{opacity:.25}}
.blink{animation:blinkkf 1s steps(2) infinite}
.wrap{min-height:100vh;display:flex;flex-direction:column}
:host([fit]){height:100%}
:host([fit]) .wrap{height:100%;min-height:0}
header{display:flex;align-items:center;gap:12px;height:40px;padding:0 12px;background:#15181B;border-bottom:1px solid #262B31;flex:none}
header>span{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;min-width:0}
header>span:first-child{flex:none}
header .wb{width:30px;height:26px;border:1px solid #2E343A;background:#1C2024;color:#C9D0D6;border-radius:4px;cursor:pointer;display:flex;align-items:center;justify-content:center;padding:0}
header .wb.set{width:auto;gap:6px;padding:0 9px;font:600 9.5px 'IBM Plex Mono',Consolas,monospace;letter-spacing:.12em}
/* the stage: everything in it is placed by the solver (_fit) */
.stage{position:relative;flex:1;min-height:0;background:#1E2226;overflow:hidden}
.unit,.prevs,.rack{position:absolute;left:0;top:0}
.unit{display:block;padding:10px;gap:0}
.bez{background:#121417;border:1px solid #3A4148;border-radius:5px;padding:8px;box-shadow:inset 0 0 0 4px #1B1F23}
.mplate{display:flex;align-items:center;gap:7px;height:18px;margin-top:6px;white-space:nowrap;overflow:hidden}
.mplate .eng{overflow:hidden;text-overflow:ellipsis}
.mplate .id{font:700 11px 'IBM Plex Mono',Consolas,monospace;color:#16191C;background:#C9D0D6;border-radius:2px;padding:0 5px;letter-spacing:.06em;flex:none}
.tally{font:700 9px 'IBM Plex Mono',Consolas,monospace;letter-spacing:.14em;color:#5A2A27;border:1px solid #3A2222;border-radius:2px;padding:1px 5px;flex:none}
.tally.on{color:#FF6B66;border-color:#E0524E;box-shadow:0 0 6px rgba(224,82,78,.45)}
.strip{display:flex;flex-wrap:wrap;column-gap:${SCG}px;row-gap:${SRG}px;margin-top:${SG}px;align-items:stretch}
.grp{background:#24292E;border:1px solid #353C43;border-radius:4px;padding:7px 9px 8px;display:flex;flex-direction:column;gap:6px;flex:none;box-shadow:inset 0 1px 0 rgba(255,255,255,.03)}
.grp .gh{display:flex;align-items:center;gap:6px;height:18px}
.grp.na .gh .plate{color:#7D8690}
.prevs{display:grid;gap:${GAP}px}
.tile{display:block;width:100%;padding:6px;cursor:pointer;text-align:left;color:inherit;font:inherit;background:#121417;border:1px solid #3A4148;border-radius:5px;box-shadow:inset 0 0 0 3px #1B1F23}
div.tile{cursor:default}
.tile .row{gap:6px;margin-top:6px;height:16px;white-space:nowrap}
.tsel{flex:1;min-width:0;padding:0;background:none;border:0;color:#AEB6BE;font:600 9.5px 'IBM Plex Mono',Consolas,monospace;letter-spacing:.14em;text-transform:uppercase;text-align:left;cursor:pointer;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.tsel:hover{color:#E6EDF3}
.tsel.dim{color:#7D8690}
.rack{display:flex;flex-wrap:wrap;gap:${RG}px;align-content:flex-start}
.rack>.pnl{flex:none}
.rack.side>.pnl{flex:1 0 auto}
.rack>.blank{display:none;flex:1 1 0;min-width:0}
.rack.fill>.blank{display:block}
.pnl>.grp.in{flex:1 1 auto}
.rack>.pnl[data-p=bb],.rack>.pnl[data-p=stream]{width:178px}
.rack.side>.pnl{align-items:stretch}
.measure .strip,.measure .rack{width:max-content!important;flex-wrap:nowrap!important}
.strip>.grp,.rack>.pnl{zoom:var(--z,1)}
.measure .strip>.grp,.measure .rack>.pnl{flex:none!important;zoom:1!important}
.measure .rack>.blank{display:none!important}
.measure .unit{width:max-content!important}
.ship{display:flex;align-items:stretch;gap:4px}
.ship .lcd{flex:1;display:flex;flex-direction:column;justify-content:center;gap:1px;cursor:pointer;width:150px;padding:3px 6px;text-align:left;font:inherit;font-family:'IBM Plex Mono',Consolas,monospace}
.ship .lcd b{font-size:13px;color:#8EE089}
.ship .lcd span{font-size:9px;color:#5E9A5A;letter-spacing:.08em;overflow:hidden;text-overflow:ellipsis;max-width:138px}
.ship.view .lcd{width:auto;min-width:0;flex-direction:row;align-items:center;justify-content:space-between;gap:8px}
.ship.view .lcd b{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.joy{position:relative;width:84px;height:84px;border-radius:6px;background:linear-gradient(#15181B,#0D0F11);border:1px solid #3A4148;box-shadow:inset 0 2px 6px rgba(0,0,0,.8);touch-action:none;cursor:grab;flex:none}
.joy svg{width:100%;height:100%}
.joy.on{cursor:grabbing}
.joy.na{opacity:.35;cursor:default}
.tgt{display:block;width:166px;padding:4px 7px;font-size:11px;text-align:left;overflow:hidden;text-overflow:ellipsis}
.grp.na .tgt{opacity:.45}
.over{position:fixed;inset:0;pointer-events:none;z-index:10}
.over>*{pointer-events:auto}
.pop{position:fixed;min-width:220px;max-height:60vh;overflow:auto;background:#1B1F23;border:1px solid #444C54;border-radius:4px;box-shadow:0 8px 24px rgba(0,0,0,.6);padding:4px}
.pop button{display:flex;align-items:center;gap:8px;width:100%;padding:6px 8px;background:none;border:0;border-radius:3px;color:#C9D0D6;cursor:pointer;font:600 11px 'IBM Plex Mono',Consolas,monospace;letter-spacing:.06em;text-align:left}
.pop button:hover{background:#2A2F35}
.pop button .h{color:#8EE089;min-width:64px}
.back{position:fixed;inset:0;background:rgba(5,6,7,.62);display:flex;align-items:center;justify-content:center}
.modal{position:fixed;left:50%;top:50%;transform:translate(-50%,-50%);max-width:calc(100vw - 48px);max-height:calc(100vh - 48px);display:flex;flex-direction:column;background:#1E2226;border:1px solid #4A525A;border-radius:6px;box-shadow:0 18px 50px rgba(0,0,0,.7)}
.mhead .hint{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.fld.path{height:40px;resize:none;line-height:1.35;white-space:pre-wrap;word-break:break-all;overflow:hidden;font-size:11px;padding:3px 6px}
.toks{display:flex;flex-wrap:wrap;gap:3px}
.tok{padding:1px 4px;border:1px solid #2E3B30;border-radius:2px;background:#0A100B;color:#7FC77A;font:600 9.5px 'IBM Plex Mono',Consolas,monospace;cursor:pointer}
.tok:hover{border-color:#5E9A5A;color:#8EE089}
.toks.multi .tok{color:#56705A;border-color:#253027;padding:2px 6px}
.toks.multi .tok.on{color:#9CF197;border-color:#5E9A5A;background:#12301A;box-shadow:0 0 6px rgba(127,199,122,.25)}
.frow:has(textarea){align-items:start}
.frow:has(textarea)>.eng{padding-top:13px}
.keyb{text-align:left;cursor:pointer;font-family:'IBM Plex Mono',Consolas,monospace;width:150px}
.keyb.listen{color:#E7B04F;border-color:#E7B04F}
.keyb.none{color:#5E9A5A}
.mhead,.mfoot{display:flex;align-items:center;gap:8px;padding:10px 14px;flex:none}
.mhead{border-bottom:1px solid #3A4148}
.mfoot{border-top:1px solid #3A4148;flex-wrap:wrap}
.mbody{flex:none;min-height:0;overflow:hidden;padding:12px 14px;display:flex;gap:12px;align-items:stretch}
.mbody.scroll{overflow-x:hidden;overflow-y:auto}
.mbody.measuring{display:block}
.mbody.measuring .sect{margin-bottom:12px}
.scol{display:flex;flex-direction:column;gap:12px;min-width:0;flex:none}
.sect{gap:8px}
.scol .sect{flex:none}
.sect.keys{gap:4px}
.sect.keys .fld{padding:2px 6px;font-size:11px}
.sect>.plate{align-self:flex-start;margin-bottom:2px}
.sect .rk{width:24px;height:34px}
.sect .rk .i{top:5px;height:6px}
.sect .rk .o{bottom:4px;width:7px;height:7px;margin-left:-3.5px}
.frow{display:grid;grid-template-columns:minmax(110px,36%) minmax(0,1fr);align-items:center;gap:4px 10px}
.frow .hint{grid-column:2}
.fld{width:100%;min-width:0;background:#0A100B;color:#8EE089;border:1px solid #2E3B30;border-radius:2px;padding:4px 6px;font-size:11.5px;font-weight:600;outline:none}
.fld:focus{border-color:#5E9A5A}
.fld[type=number]{appearance:textfield;-moz-appearance:textfield}
.fld::-webkit-outer-spin-button,.fld::-webkit-inner-spin-button{-webkit-appearance:none;margin:0}
select.fld{appearance:auto}
.unitx{font-size:9.5px;color:#7D8690;white-space:nowrap}
.rs{color:#E7B04F;font-size:11px;margin-left:4px}
.banner[hidden]{display:none}
.kc.sm{height:28px;padding-top:0}
.banner{margin:0 14px;padding:7px 10px;border-radius:3px;background:#2E2716;border:1px solid #6B5422;color:#E7B04F;font:600 10px 'IBM Plex Mono',Consolas,monospace;letter-spacing:.08em;display:flex;align-items:center;gap:10px;white-space:pre-line;line-height:1.5}
`;

  const SCREWS = '<i class="scr" style="left:5px;top:5px"></i><i class="scr" style="right:5px;top:5px"></i><i class="scr" style="left:5px;bottom:5px"></i><i class="scr" style="right:5px;bottom:5px"></i>';
  // a key binding's name: virtual key | 0x100 Shift | 0x200 Ctrl | 0x400 Alt
  const keyName = code => {
    if (!code) return 'None';
    const vk = code & 0xFF, names = { 8: 'Backspace', 9: 'Tab', 13: 'Enter', 19: 'Pause', 27: 'Esc', 32: 'Space', 33: 'PgUp', 34: 'PgDn', 35: 'End', 36: 'Home',
      37: '←', 38: '↑', 39: '→', 40: '↓', 45: 'Ins', 46: 'Del', 106: 'Num *', 107: 'Num +', 109: 'Num −', 110: 'Num .', 111: 'Num /',
      186: ';', 187: '+', 188: ',', 189: '−', 190: '.', 191: '/', 192: '`', 219: '[', 220: '\\', 221: ']', 222: "'" };
    let n = names[vk];
    if (!n && vk >= 48 && vk <= 57 || vk >= 65 && vk <= 90) n = String.fromCharCode(vk);
    if (!n && vk >= 96 && vk <= 105) n = 'Num ' + (vk - 96);
    if (!n && vk >= 112 && vk <= 135) n = 'F' + (vk - 111);
    return (code & 0x200 ? 'Ctrl+' : '') + (code & 0x100 ? 'Shift+' : '') + (code & 0x400 ? 'Alt+' : '') + (n || 'Key ' + vk);
  };
  const esc = s => String(s == null ? '' : s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  // keycap: lamp = colour name or '' (momentary key, no lamp)
  const key = (attr, label, k, lamp, cls) => `<button class="kc${lamp ? '' : ' plain'}${cls ? ' ' + cls : ''}" ${attr}${lamp ? ` data-lamp="${lamp}"` : ''}>${lamp ? '<i class="lamp"></i>' : ''}${label}${k ? `<span class="k">${k}</span>` : ''}</button>`;
  const rocker = (attr, label) => `<button class="rk" ${attr} aria-label="${esc(label)}" aria-pressed="false"><span class="face"><i class="i"></i><i class="o"></i></span></button>`;
  const GEAR = '<svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="3.2"></circle><path d="M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z"></path></svg>';
  const JOY = u => `<svg viewBox="-50 -50 100 100" aria-hidden="true">
<defs><radialGradient id="jb${u}" cx="45%" cy="40%" r="60%"><stop offset="0" stop-color="#3A3F45"></stop><stop offset="1" stop-color="#0B0D0F"></stop></radialGradient>
<radialGradient id="jk${u}" cx="38%" cy="32%" r="70%"><stop offset="0" stop-color="#FAFBFC"></stop><stop offset=".45" stop-color="#A9B0B7"></stop><stop offset="1" stop-color="#3E444A"></stop></radialGradient></defs>
<g stroke="#4A525A" stroke-width="1.2"><line x1="0" y1="-46" x2="0" y2="-40"></line><line x1="0" y1="40" x2="0" y2="46"></line><line x1="-46" y1="0" x2="-40" y2="0"></line><line x1="40" y1="0" x2="46" y2="0"></line></g>
<circle r="32" fill="url(#jb${u})" stroke="#050607" stroke-width="1"></circle><circle r="25" fill="none" stroke="#25292E" stroke-width="2"></circle><circle r="18" fill="none" stroke="#25292E" stroke-width="2"></circle><circle r="11" fill="#14171A"></circle>
<line class="shaft" x1="0" y1="0" x2="0" y2="0" stroke="#8C949C" stroke-width="5" stroke-linecap="round"></line>
<circle class="ball" r="11" fill="url(#jk${u})" stroke="#23272B" stroke-width="1"></circle></svg>`;
  // what lies on a monitor's screen: test bars and message, CCTV stand-in, crosshair, burned-in text
  const MONOV = id => `<div class="ovroot" data-ov="${id}">
<div class="vid"></div><div class="nosig" hidden><div class="bars"></div><span class="burn msg" data-o="msg" style="left:50%;top:50%;transform:translate(-50%,-50%);font-size:20px">NO SIGNAL</span></div>
<div class="ovl look" style="background:repeating-linear-gradient(0deg,rgba(0,0,0,.55) 0 1px,transparent 1px 3px)"></div>
<div class="ovl look" style="background:radial-gradient(ellipse at center,transparent 55%,rgba(0,0,0,.6) 100%)"></div>
<div class="ovl cross" hidden><span style="position:absolute;left:50%;top:0;bottom:0;width:1px;background:rgba(255,255,255,.55)"></span><span style="position:absolute;top:46%;left:0;right:0;height:1px;background:rgba(255,255,255,.55)"></span></div>
<span class="burn" data-o="time" style="left:14px;top:10px;font-size:15px"></span>
<span class="burn" data-o="ship" style="right:14px;top:10px;font-size:15px"></span>
<span class="burn" data-o="name" style="left:14px;bottom:10px;font-size:15px"></span>
<span class="burn" data-o="rec" style="right:14px;bottom:10px;font-size:15px"></span></div>`;

  // Greedy rows, as flex-wrap breaks them: the height of items [w, h] packed into `width` (Infinity: one is wider).
  const pack = (items, width, cg, rg) => {
    let h = 0, rows = 0, rowW = 0, rowH = 0;
    for (const [w, hh] of items) {
      if (w > width) return Infinity;
      if (rowW && rowW + cg + w > width) { h += rowH; rows++; rowW = 0; rowH = 0; }
      rowW = rowW ? rowW + cg + w : w;
      rowH = Math.max(rowH, hh);
    }
    if (rowW) { h += rowH; rows++; }
    return h + Math.max(0, rows - 1) * rg;
  };

  class IlartsConsole extends HTMLElement {
    constructor() {
      super();
      this._state = null;
      this._videos = {};
      this._listSig = '';
      this._formRev = -1;
      this._sizes = null;  // measured sizes of the strip groups and rack modules (_measure)
      this.attachShadow({ mode: 'open' });
    }

    connectedCallback() {
      if (!this._built) this._build();
      this._ro = new ResizeObserver(() => this._layout());
      this._ro.observe(this);
      window.addEventListener('scroll', this._onScroll = () => this._layout(), true);
      this._layout();
    }
    disconnectedCallback() {
      if (this._ro) this._ro.disconnect();
      window.removeEventListener('scroll', this._onScroll, true);
    }

    set state(s) {
      this._state = s;
      if (this._built) this._update();
    }
    get state() { return this._state; }
    get modalOpen() { return !!this.shadowRoot.querySelector('.over .modal'); }

    // Web hosts: the element (a <video>, <img>, <canvas>) shown in monitor `id` ('program', 'second' or a source id).
    setVideo(id, element) {
      this._videos[id] = element || null;
      const holder = this.shadowRoot.querySelector(`[data-ov="${id}"] .vid`);
      if (holder) {
        holder.textContent = '';
        if (element) holder.appendChild(element);
      }
    }

    // The host answers a settings action (a folder picked ...): the field shows the new value.
    setSettingsField(key, value) {
      const f = this.shadowRoot.querySelector(`.over [data-k="${key}"]`);
      if (f) f.value = value;
    }

    _cmd(cmd, extra) {
      this.dispatchEvent(new CustomEvent('command', { detail: Object.assign({ cmd }, extra || {}), bubbles: true, composed: true }));
    }

    // ---- the state, read with defaults
    // The monitors: state.monitors, or (older hosts) one monitor made of state.program and state.picture.
    _mons() {
      const s = this._state || {};
      if (Array.isArray(s.monitors) && s.monitors.length) return s.monitors;
      return [Object.assign({}, s.program || {}, { picture: s.picture || {}, ptz: null })];
    }
    _wall() { const w = this._state && this._state.layout; return w === '1' || w === '2' || w === 'q' ? w : 'q'; }

    _build() {
      this._built = true;
      // Desktop: the pictures are drawn BEHIND the page, so the page is cut open at the screens (clip-path, set in
      // _layout) and what lies on a screen - text, bars - sits in a layer above the cut (.top). Pop-ups and the
      // settings dialog are in .over, above everything.
      const native = this._native = this.hasAttribute('native-video');
      const knob = (u, k) => `<div class="kn" data-knob="${k}"><svg width="42" height="42" viewBox="-30 -30 60 60" role="slider" tabindex="-1" aria-label="${KNOBS[k][0]}">
<circle r="27" fill="#14171A" stroke="#0A0B0D" stroke-width="1"></circle><g stroke="#59616A" stroke-width="1.2">${TICKS}</g>
<circle r="20" fill="#1E2227" stroke="#3B424A" stroke-width="1.5"></circle><circle r="15" fill="#262B31"></circle>
<line class="ptr" x1="0" y1="-6" x2="0" y2="-19" stroke="#E6EDF3" stroke-width="2.6" stroke-linecap="round"></line></svg>
<span class="lcd val"></span><span class="eng" style="font-size:8px">${KNOBS[k][0]}</span></div>`;
      // A monitor and its channel strip: every control here acts on what this monitor shows.
      const unit = u => `<section class="pnl unit" data-unit="${u}" aria-label="Monitor ${u ? 'B' : 'A'}">${SCREWS}
 <div class="bez"><div class="scrn" data-mon="${MONS[u]}">${native ? '' : MONOV(MONS[u])}</div>
  <div class="mplate"><span class="id">${u ? 'B' : 'A'}</span><span class="led" data-o="sig"></span><span class="eng" data-o="plate"></span><span class="sp"></span><span class="eng dim" data-o="res"></span><span class="tally" data-o="tally" title="The VTR is recording this picture">REC</span></div></div>
 <div class="strip">
  <div class="grp" data-g="src"><div class="gh"><span class="plate">Source</span><span class="sp"></span>${key('data-b="recArm" title="The VTR records this source"', 'REC', '', 'red', 'mini')}</div><div class="row" data-l="srcs" style="gap:5px"></div>
   <div class="ship view" data-l="view">${key('data-b="viewPrev" aria-label="Previous view"', '◀', '', '', 'arw')}<button class="lcd" data-b="viewList" aria-label="Choose the view"><b data-o="view"></b><span data-o="viewN"></span></button>${key('data-b="viewNext" aria-label="Next view"', '▶', '', '', 'arw')}</div></div>
  <div class="grp" data-g="lev"><div class="gh"><span class="plate">Levels &middot; <span data-o="group"></span></span><span class="sp"></span><span class="eng dim" style="font-size:8px">Auto</span><span class="lcd sm" data-o="autoRange"></span></div>
   <div class="row" style="align-items:flex-start;gap:6px"><div class="col" style="gap:5px">${key('data-b="auto"', 'AUTO', u ? '' : 'A', 'green')}${key('data-b="reset"', 'RESET', u ? '' : 'R', '', 'warn')}</div>${knob(u, 'exp')}${knob(u, 'bri')}${knob(u, 'con')}${knob(u, 'gam')}</div></div>
  <div class="grp" data-g="ptz"><div class="gh"><span class="plate">PTZ &middot; <span data-o="cam"></span></span></div>
   <div class="row" style="align-items:flex-start;gap:8px"><div class="joy" data-joy title="Drag: pan / tilt · hold left: zoom in · hold right: zoom out · wheel: zoom">${JOY(u)}</div>
   <div class="col" style="gap:6px;align-items:flex-start"><div class="row" style="gap:5px">${key('data-b="ptzRest" title="Back to the rest view, and stay there"', 'REST', '', '')}${key('data-b="ptzAuto" title="Back to the rest view, then follow the target"', 'AUTO', '', 'green')}${key('data-b="ptzTarget" title="What AUTO follows (next)"', 'TARGET', '', '')}${key('data-b="ptzFlir" title="Experimental: DCS\'s infrared picture for this camera. Costs DCS frame rate" hidden', 'FLIR', '', 'amber')}</div><span class="lcd tgt" data-o="target"></span></div></div></div>
 </div>
</section>`;
      this.shadowRoot.innerHTML = `<style>${CSS}</style>
<div class="wrap">
 <header data-drag>
  <span style="font-weight:700;letter-spacing:.1em;font-size:14px" data-drag>DCS ILARTS</span>
  <span class="mono dim" style="font-size:11px" data-drag>INTEGRATED LAUNCH AND RECOVERY TELEVISION SURVEILLANCE</span>
  <div class="sp" data-drag></div>
  <span class="lcd" data-t="status"></span>
  <div class="winbtns" style="display:flex;gap:4px">
   <button class="wb set" data-b="settings" aria-label="Settings">${GEAR}SETTINGS</button>
   <span class="wbx" style="display:flex;gap:4px">
   <button class="wb" data-win="min" aria-label="Minimise"><svg width="12" height="12" viewBox="0 0 12 12"><path d="M2 9h8" stroke="currentColor" stroke-width="1.5"></path></svg></button>
   <button class="wb" data-win="max" aria-label="Maximise"><svg width="12" height="12" viewBox="0 0 12 12"><rect x="2.5" y="2.5" width="7" height="7" fill="none" stroke="currentColor" stroke-width="1.3"></rect></svg></button>
   <button class="wb" data-win="close" aria-label="Close"><svg width="12" height="12" viewBox="0 0 12 12"><path d="M3 3l6 6M9 3l-6 6" stroke="currentColor" stroke-width="1.5"></path></svg></button>
   </span>
  </div>
 </header>
 <div class="stage">
  ${unit(0)}${unit(1)}
  <div class="prevs" data-l="prevs" aria-label="Previews"></div>
  <section class="rack" aria-label="Shared controls">
   <div class="pnl blank" aria-hidden="true">${SCREWS}</div>
   <div class="pnl" data-p="wall">${SCREWS}
    <div class="row"><span class="plate">Monitors</span></div>
    <div style="display:grid;grid-template-columns:repeat(2,52px);gap:5px">${key('data-wall="1" title="One monitor"', '1', '', 'white')}${key('data-wall="2" title="Two monitors, each with its own controls"', '2', '', 'white')}${key('data-wall="q" title="One monitor and the other sources small"', '1 + 4', '', 'white')}${key('data-b="swap" title="Swap the pictures of monitors A and B"', 'A ⇄ B', '', '')}</div>
   </div>
   <div class="pnl" data-p="ship">${SCREWS}
    <div class="row"><span class="plate">Ship</span><div class="sp"></div><span class="hint" data-t="shipHint"></span></div>
    <div class="ship">${key('data-b="shipPrev" aria-label="Previous ship"', '◀', '', '', 'arw')}<button class="lcd" data-b="shipList" aria-label="Choose the ship"><b data-t="shipHull"></b><span data-t="shipUnit"></span></button>${key('data-b="shipNext" aria-label="Next ship"', '▶', 'N', '', 'arw')}</div>
   </div>
   <div class="pnl" data-p="presets">${SCREWS}
    <div class="row"><span class="plate">Presets</span></div>
    <div data-l="presets" style="display:grid;grid-template-columns:repeat(4,58px);gap:5px"></div>
    <span class="hint" data-t="presetHint" style="width:247px;height:12px;overflow:hidden;white-space:nowrap;text-overflow:ellipsis"></span>
   </div>
   <div class="pnl" data-p="vtr">${SCREWS}
    <div class="row"><span class="plate">VTR remote</span><div class="sp"></div><span class="lcd" data-t="counter" style="min-width:84px"></span></div>
    <div class="row" style="align-items:flex-start;gap:10px">
     <div class="col" style="gap:7px">
      <div class="lcd" style="display:block;text-align:left;line-height:1.45;font-size:10.5px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;width:310px"><span data-t="vtr1"></span><br><span data-t="vtr2"></span></div>
      <div class="row" style="gap:4px" title="The sources the VTR records (one file each)"><span class="eng dim" style="font-size:8px;width:26px;line-height:1.25">REC<br>SRC</span><div class="row" data-l="recs" style="gap:5px"></div></div>
     </div>
     <div style="display:grid;grid-template-columns:repeat(4,52px);gap:5px">${key('data-b="manual"', 'MAN', '', 'amber')}${key('data-b="automode"', 'AUTO', 'M', 'amber')}${key('data-b="rec"', '● REC', 'V', 'red')}${key('data-b="play"', '▶ PLAY', 'SPC', 'white')}${key('data-b="stop"', '■ STOP', '', '')}${key('data-b="save"', 'SAVE', 'S', '')}${key('data-b="folder"', 'FOLDER', '', '', 'w" style="grid-column:span 2;width:auto')}</div>
    </div>
   </div>
   <div class="pnl" data-p="bb">${SCREWS}
    <div class="row"><span class="plate">Black box</span></div>
    <div class="row" style="gap:10px">${rocker('data-b="bb"', 'Black box')}<span class="led" data-led="bb"></span><div class="sp"></div>${key('data-b="bbSave"', 'SAVE', 'B', '')}</div>
    <span class="hint" data-t="bbInfo" style="max-height:37px;overflow:hidden"></span>
   </div>
   <div class="pnl" data-p="stream">${SCREWS}
    <div class="row"><span class="plate">Stream</span></div>
    <div class="row" style="gap:10px">${rocker('data-b="stream"', 'Stream')}<span class="led" data-led="stream"></span><span class="eng" data-t="streamWord" style="font-size:9px"></span></div>
    <div style="display:grid;grid-template-columns:repeat(4,1fr);gap:5px">${[0, 1, 2, 3].map(n => key(`data-stn="${n}" aria-label="Stream ${n + 1}"`, String(n + 1), '', 'red', 'mini" style="width:auto')).join('')}</div>
    <span class="hint" data-t="streamInfo" style="max-height:37px;overflow:hidden;white-space:pre-line"></span>
   </div>
   <div class="pnl blank" aria-hidden="true">${SCREWS}</div>
  </section>
 </div>
</div><div class="top">${native ? MONS.map(MONOV).join('') : ''}</div><div class="over"></div>`;
      const root = this.shadowRoot;
      root.querySelectorAll('.rack>.pnl:not(.blank)').forEach(p => {
        const tray = document.createElement('div');
        tray.className = 'grp in';
        [...p.children].filter(c => !c.classList.contains('scr')).forEach(c => tray.appendChild(c));
        p.appendChild(tray);
      });
      this._t = {};
      root.querySelectorAll('[data-t]').forEach(e => { this._t[e.dataset.t] = e; });
      this._over = root.querySelector('.over');
      this._units = [...root.querySelectorAll('.unit')];
      root.addEventListener('click', e => this._click(e));
      root.addEventListener('contextmenu', e => {
        e.preventDefault();
        const p = e.target.closest('[data-preset]');
        if (p) this._cmd('presetMenu', { index: +p.dataset.preset, x: e.clientX, y: e.clientY });
      });
      root.addEventListener('mousedown', e => {
        if (e.button === 0 && e.target.closest('[data-drag]') && !e.target.closest('button')) this._cmd('drag');
        const kn = e.target.closest('[data-knob]');
        if (kn && e.button === 0) this._knobDrag(this._unitOf(kn), kn.dataset.knob, e);
      });
      root.addEventListener('dblclick', e => {
        if (e.target.closest('[data-drag]') && !e.target.closest('button')) this._cmd('win', { what: 'max' });
      });
      root.addEventListener('wheel', e => {
        const kn = e.target.closest('[data-knob]');
        if (!kn || !this._state) return;
        e.preventDefault();
        const u = this._unitOf(kn), k = kn.dataset.knob, K = this._K(k), dir = e.deltaY < 0 ? 1 : -1;
        if (this._na(u, k)) return;
        const x = this._knobValue(u, k);
        this._setKnob(u, k, K.log ? x * Math.pow(1.05, dir * K.notch) : x + dir * K.step * K.notch);
      }, { passive: false });
      this._units.forEach((el, u) => this._joySetup(el.querySelector('[data-joy]'), u));
      document.addEventListener('keydown', e => { if (e.key === 'Escape' && this.modalOpen) { e.stopPropagation(); this._closeOver(true); } }, true);
      document.fonts && document.fonts.ready.then(() => { this._sizes = null; this._layout(); });
      if (this._state) this._update();
    }

    _unitOf(el) { const u = el.closest('[data-unit]'); return u ? +u.dataset.unit : 0; }

    // ---- knobs (each monitor's knobs set the levels of the picture it shows)
    _K(k) {
      const d = KNOBS[k], o = (this._state && this._state.knobs && this._state.knobs[k]) || {};
      const num = (v, def) => (typeof v === 'number' && isFinite(v) ? v : def);
      return { label: d[0], min: num(o.min, d[1]), max: num(o.max, d[2]), step: num(o.step, d[3]), fmt: d[4], notch: d[5], log: !!o.log };
    }
    _pos(K, x) {
      x = Math.max(K.min, Math.min(K.max, x));
      return K.log ? Math.log(x / K.min) / Math.log(K.max / K.min) : (x - K.min) / (K.max - K.min);
    }
    _at(K, t) {
      t = Math.max(0, Math.min(1, t));
      return K.log ? K.min * Math.pow(K.max / K.min, t) : K.min + t * (K.max - K.min);
    }
    _pic(u) { const m = this._mons()[u]; return (m && m.picture) || null; }
    _na(u, k) { const p = this._pic(u); return !p || !!(p.unavailable && p.unavailable[k]); }
    _knobValue(u, k) { const p = this._pic(u); return p ? +p[k] || 0 : 0; }
    _setKnob(u, k, x) {
      const K = this._K(k);
      x = Math.max(K.min, Math.min(K.max, Math.round(x / K.step) * K.step));
      const s = this._state || {};
      if (Array.isArray(s.monitors) && s.monitors[u] && s.monitors[u].picture) s.monitors[u].picture[k] = x;  // shown at once; the host's next state confirms
      else if (s.picture && u === 0) s.picture[k] = x;
      this._update();
      this._cmd('knob', { key: k, value: +x.toFixed(4), mon: u });
    }
    _knobDrag(u, k, e) {
      e.preventDefault();
      if (this._na(u, k)) return;
      const K = this._K(k), y0 = e.clientY, t0 = this._pos(K, this._knobValue(u, k));
      const mv = ev => this._setKnob(u, k, this._at(K, t0 + (y0 - ev.clientY) / 160));
      const up = () => { window.removeEventListener('mousemove', mv, true); window.removeEventListener('mouseup', up, true); };
      window.addEventListener('mousemove', mv, true);
      window.addEventListener('mouseup', up, true);
    }

    // ---- PTZ joystick (one per monitor; it moves the camera that monitor shows): left drag = pan / tilt (rate
    // follows the deflection, springs back on release), left held still = zoom in, right held = zoom out, wheel =
    // a short zoom. Rates go to the host 10 times a second while held ({cmd:'ptz', cam, pan, tilt, zoom}, -1 .. 1),
    // then once more as zeros.
    _joyCam(u) { const m = this._mons()[u]; return (m && m.ptz) || null; }
    _joySetup(el, u) {
      const R = 30;  // px of deflection for full rate
      const draw = (x, y) => {
        const sh = el.querySelector('.shaft'), b = el.querySelector('.ball');
        sh.setAttribute('x2', (x * 24).toFixed(1)); sh.setAttribute('y2', (y * 24).toFixed(1));
        b.setAttribute('cx', (x * 24).toFixed(1)); b.setAttribute('cy', (y * 24).toFixed(1));
      };
      let j = null;
      const send = () => {
        if (!j) return;
        if (j.btn === 0 && !j.drag && performance.now() - j.t0 > 220) j.zoom = 1;
        this._cmd('ptz', { cam: j.cam, pan: +j.x.toFixed(3), tilt: +(-j.y).toFixed(3), zoom: j.zoom });
      };
      const end = () => {
        if (!j) return;
        clearInterval(j.timer);
        this._cmd('ptz', { cam: j.cam, pan: 0, tilt: 0, zoom: 0 });
        j = null;
        el.classList.remove('on');
        const sh = el.querySelector('.shaft'), x = +sh.getAttribute('x2') / 24, y = +sh.getAttribute('y2') / 24, t0 = performance.now();
        const step = () => {  // spring back
          const k = Math.min(1, (performance.now() - t0) / 120);
          draw(x * (1 - k), y * (1 - k));
          if (k < 1 && !j) requestAnimationFrame(step);
        };
        requestAnimationFrame(step);
      };
      el.addEventListener('pointerdown', e => {
        const cam = this._joyCam(u);
        if (!cam || !cam.live || (e.button !== 0 && e.button !== 2)) return;
        e.preventDefault();
        el.setPointerCapture(e.pointerId);
        el.classList.add('on');
        j = { cam: cam.id, btn: e.button, x0: e.clientX, y0: e.clientY, x: 0, y: 0, zoom: e.button === 2 ? -1 : 0, drag: false, t0: performance.now() };
        j.timer = setInterval(send, 100);
        send();
      });
      el.addEventListener('pointermove', e => {
        if (!j || j.btn !== 0) return;
        const dx = e.clientX - j.x0, dy = e.clientY - j.y0;
        if (!j.drag && Math.hypot(dx, dy) > 4) { j.drag = true; j.zoom = 0; }
        if (!j.drag) return;
        let x = dx / R, y = dy / R;
        const m = Math.hypot(x, y);
        if (m > 1) { x /= m; y /= m; }
        j.x = x; j.y = y;
        draw(x, y);
        send();
      });
      el.addEventListener('pointerup', end);
      el.addEventListener('pointercancel', end);
      el.addEventListener('lostpointercapture', end);
      el.addEventListener('wheel', e => {
        const cam = this._joyCam(u);
        e.preventDefault();
        if (!cam || !cam.live || j) return;
        this._cmd('ptz', { cam: cam.id, pan: 0, tilt: 0, zoom: e.deltaY < 0 ? 1 : -1 });
        clearTimeout(el._wheelZ);
        el._wheelZ = setTimeout(() => this._cmd('ptz', { cam: cam.id, pan: 0, tilt: 0, zoom: 0 }), 200);
      }, { passive: false });
    }

    // ---- pop-ups (ship list) and the settings dialog, in .over
    _closeOver(tellHost) {
      const wasModal = this.modalOpen;
      this._over.textContent = '';
      if (wasModal && tellHost) this._cmd('settingsClose');
      this._formRev = -1;
    }
    _shipList(anchor) {
      const ships = (this._state && this._state.ships) || [];
      if (!ships.length) return;
      const r = anchor.getBoundingClientRect();
      this._over.innerHTML = `<div class="back" data-close style="background:transparent"></div><div class="pop" role="listbox">${ships.map(s => `<button data-pick-ship="${esc(s.id)}"><span class="led" style="background:${LED.green[s.on ? 0 : 1]}"></span><span class="h">${esc(s.hull)}</span><span>${esc(s.unit)}</span></button>`).join('')}</div>`;
      const pop = this._over.querySelector('.pop');
      const h = pop.getBoundingClientRect().height;
      const above = r.top - h - 6 > 0 && r.bottom + h + 6 > innerHeight;
      Object.assign(pop.style, { left: Math.max(4, Math.min(innerWidth - pop.offsetWidth - 4, r.left)) + 'px', top: (above ? r.top - h - 6 : r.bottom + 6) + 'px' });
      this._over.onclick = e => {
        const b = e.target.closest('[data-pick-ship]');
        if (b) this._cmd('ship', { id: b.dataset.pickShip });
        if (b || e.target.closest('[data-close]')) this._closeOver(false);
      };
    }
    // A pop-up list under (or above) anchor: items [{label, on}], onPick(index).
    _pickList(anchor, items, onPick) {
      if (!items.length) return;
      const r = anchor.getBoundingClientRect();
      this._over.innerHTML = `<div class="back" data-close style="background:transparent"></div><div class="pop" role="listbox">${items.map((it, i) => `<button data-pick-item="${i}"><span class="led" style="background:${LED.green[it.on ? 0 : 1]}"></span><span>${esc(it.label)}</span></button>`).join('')}</div>`;
      const pop = this._over.querySelector('.pop');
      const h = pop.getBoundingClientRect().height;
      const above = r.top - h - 6 > 0 && r.bottom + h + 6 > innerHeight;
      Object.assign(pop.style, { left: Math.max(4, Math.min(innerWidth - pop.offsetWidth - 4, r.left)) + 'px', top: (above ? r.top - h - 6 : r.bottom + 6) + 'px' });
      this._over.onclick = e => {
        const b = e.target.closest('[data-pick-item]');
        if (b) onPick(+b.dataset.pickItem);
        if (b || e.target.closest('[data-close]')) this._closeOver(false);
      };
    }
    // a monitor's deck position
    _viewList(anchor, m, u) {
      const views = (m && m.views) || [];
      this._pickList(anchor, views.map((v, i) => ({ label: v, on: i === m.view })), i => this._cmd('view', { mon: u, index: i }));
    }
    // a preview's source: none, or any source
    _tileList(anchor, slot) {
      const s = this._state || {}, sources = s.sources || [], cur = (s.previews || [])[slot] || '';
      const items = [{ id: '', label: 'No source', on: !cur }].concat(sources.map(x => ({ id: x.id, label: x.plate || x.name, on: x.id === cur })));
      this._pickList(anchor, items, i => this._cmd('preview', { slot, id: items[i].id }));
    }
    _settings(f) {
      if (!f) { if (this.modalOpen) this._closeOver(false); return; }
      if (f.rev === this._formRev) { this._settingsBanner(f); return; }
      this._formRev = f.rev;
      const field = d => {
        const k = esc(d.key), v = d.value == null ? '' : d.value;
        let ctl;
        if (d.type === 'bool') ctl = `<div class="row">${rocker(`data-k="${k}" data-bool`, d.label).replace('aria-pressed="false"', `aria-pressed="${v ? 'true' : 'false'}"`)}</div>`;
        else if (d.type === 'select') ctl = `<select class="fld" data-k="${k}">${(d.options || []).map(o => `<option value="${esc(o[0])}"${String(o[0]) === String(v) ? ' selected' : ''}>${esc(o[1])}</option>`).join('')}</select>`;
        else if (d.type === 'path') ctl = `<div class="row"><textarea class="fld path" data-k="${k}" rows="2" spellcheck="false">${esc(v)}</textarea>${key(`data-sa="browse" data-key="${k}"`, 'BROWSE', '', '', 'w')}</div>`;
        else if (d.type === 'text' && (d.options || []).length) ctl = `<div class="col" style="gap:5px"><textarea class="fld path" data-k="${k}" rows="2" spellcheck="false">${esc(v)}</textarea><div class="toks">${d.options.map(o => `<button class="tok" data-tok="${esc(o[0])}" data-for="${k}" title="${esc(o[1])}">${esc(o[0])}</button>`).join('')}</div></div>`;
        else if (d.type === 'multi') { const on = String(v).split(',').filter(Boolean); ctl = `<div class="toks multi" data-k="${k}" data-multi>${(d.options || []).map(o => `<button class="tok${on.includes(String(o[0])) ? ' on' : ''}" data-m="${esc(o[0])}">${esc(o[1])}</button>`).join('')}</div>`; }
        else if (d.type === 'action') { const a = String(v).split('|'); ctl = `<div class="row">${key(`data-sa="${esc(a[0])}"${a[0] === 'none' || a[0] === 'busy' ? ' disabled' : ''}`, esc(a[1] || a[0]), '', '', 'w')}</div>`; }
        else if (d.type === 'key') ctl = `<button class="fld keyb${+v ? '' : ' none'}" data-k="${k}" data-code="${+v || 0}">${esc(keyName(+v))}</button>`;
        else if (d.type === 'number') ctl = `<div class="row"><input class="fld" data-k="${k}" type="number" value="${esc(v)}"${d.min != null ? ` min="${d.min}"` : ''}${d.max != null ? ` max="${d.max}"` : ''}${d.step != null ? ` step="${d.step}"` : ''} style="width:110px"><span class="unitx">${esc(d.unit || '')}</span></div>`;
        else ctl = `<input class="fld" data-k="${k}" type="${d.type === 'password' ? 'password' : 'text'}" value="${esc(v)}" spellcheck="false" autocomplete="off">`;
        const exp = String(d.key).startsWith('experimental.') ? '<span class="rs" style="color:#F2A03C" title="Experimental: may cost DCS frame rate or misbehave">⚠</span>' : '';
        return `<div class="frow"${d.hint ? ` title="${esc(d.hint)}"` : ''}><span class="eng" style="font-size:9px">${esc(d.label)}${exp}${d.restart ? '<span class="rs" title="Restart required">↻</span>' : ''}</span>${ctl}</div>`;
      };
      // the stream targets ("Stream 1" ...) share one panel: keys 1-4 choose the one shown
      const secs = f.sections || [], tgt = secs.filter(x => /^Stream \d$/.test(x.title));
      const one = x => `<div class="pnl sect${(x.fields || []).some(y => y.type === 'key') ? ' keys' : ''}">${SCREWS}<span class="plate">${esc(x.title)}</span>${(x.fields || []).map(field).join('')}</div>`;
      const targets = () => `<div class="pnl sect">${SCREWS}<div class="row"><span class="plate">Streams</span><div class="sp"></div>${tgt.map((x, n) => key(`data-stg-key="${n}" aria-label="${esc(x.title)}"`, String(n + 1), '', 'green', 'mini" style="width:36px')).join('')}</div>${tgt.map((x, n) => `<div class="stg" data-stg="${n}"${n ? ' hidden' : ''}>${(x.fields || []).map(field).join('')}</div>`).join('')}</div>`;
      let panels = '', done = false;
      for (const x of secs) {
        if (!tgt.includes(x)) panels += one(x);
        else if (!done) { panels += targets(); done = true; }
      }
      this._over.innerHTML = `<div class="back"><div class="modal" role="dialog" aria-label="Settings">
<div class="mhead"><span class="plate">Settings</span><span class="hint">${esc(f.file || '')}</span><div class="sp"></div><span class="hint"><span class="rs">↻</span> Restart required</span>${key('data-sa="cancel" aria-label="Close"', '✕', '', '', 'w sm')}</div>
<div class="banner" data-banner hidden></div>
<div class="mbody">${panels}</div>
<div class="mfoot">${key('data-sa="openFile" title="The settings file itself, for backup or to share"', 'OPEN FILE', '', '', 'w')}${key('data-sa="export"', 'EXPORT…', '', '', 'w')}${key('data-sa="import"', 'IMPORT…', '', '', 'w')}<div class="sp"></div>${key('data-sa="cancel"', 'CANCEL', '', '', 'w')}${key('data-sa="save"', 'SAVE', '', '', 'w')}</div>
</div></div>`;
      this._settingsBanner(f);
      const stgShow = n => {
        this._over.querySelectorAll('[data-stg]').forEach(g => { g.hidden = +g.dataset.stg !== n; });
        this._over.querySelectorAll('[data-stg-key]').forEach(b => this._lit(b, +b.dataset.stgKey === n, false));
      };
      stgShow(0);
      this._settingsLayout();
      this._over.onclick = e => {
        const sk = e.target.closest('[data-stg-key]');
        if (sk) return stgShow(+sk.dataset.stgKey);
        const b = e.target.closest('[data-bool]');
        if (b) { b.setAttribute('aria-pressed', b.getAttribute('aria-pressed') === 'true' ? 'false' : 'true'); return; }
        const mt = e.target.closest('[data-m]');
        if (mt) {  // a choice of a multi field (at most four)
          const on = mt.classList.contains('on');
          if (on || mt.parentElement.querySelectorAll('.tok.on').length < 4) mt.classList.toggle('on', !on);
          return;
        }
        const tk = e.target.closest('[data-tok]');
        if (tk) {  // a parameter into its field, at the cursor
          const ta = this._over.querySelector(`[data-k="${tk.dataset.for}"]`);
          if (!ta) return;
          const a = ta.selectionStart ?? ta.value.length, z = ta.selectionEnd ?? a, t = tk.dataset.tok;
          const pre = ta.value.slice(0, a), post = ta.value.slice(z);
          const ins = (pre && !pre.endsWith(' ') ? ' ' : '') + t + (post && !post.startsWith(' ') ? ' ' : '');
          ta.value = pre + ins + post;
          ta.focus();
          ta.selectionStart = ta.selectionEnd = (pre + ins).length;
          return;
        }
        const kb = e.target.closest('.keyb');
        if (kb) return this._listenKey(kb);
        const a = e.target.closest('[data-sa]');
        if (!a) return;
        const id = a.dataset.sa;
        if (id === 'cancel') return this._closeOver(true);
        if (id === 'save') {
          const values = {};
          this._over.querySelectorAll('[data-k]').forEach(el => {
            values[el.dataset.k] = el.hasAttribute('data-multi') ? [...el.querySelectorAll('.tok.on')].map(b => b.dataset.m).join(',') : el.hasAttribute('data-bool') ? el.getAttribute('aria-pressed') === 'true' : el.dataset.code != null ? Number(el.dataset.code) : el.type === 'number' ? Number(el.value) : el.value;
          });
          return this._cmd('settingsSave', { values });
        }
        this._cmd('settingsAction', { id, key: a.dataset.key || '' });
      };
    }
    // The settings panels in columns of 420 px: as few columns as fit the window's height (less 24 px around), in the
    // panels' order where that fits, else balanced by height. The dialog takes the size of its contents, centred;
    // only a window too small for the panels scrolls.
    _settingsLayout() {
      const body = this._over.querySelector('.mbody'), modal = this._over.querySelector('.modal');
      if (!body || !modal) return;
      const panels = [...body.querySelectorAll('.sect')];
      if (!panels.length) return;
      const banner = this._over.querySelector('[data-banner]');
      // only when the window or the banner changed (moving the panels would take the focus from a field)
      const sig = innerWidth + 'x' + innerHeight + ':' + panels.length + ':' + (banner && !banner.hidden);
      if (body._sig === sig) return;
      body._sig = sig;
      const focused = this.shadowRoot.activeElement;
      const G = 12, cs = getComputedStyle(body);
      const padX = parseFloat(cs.paddingLeft) + parseFloat(cs.paddingRight), padY = parseFloat(cs.paddingTop) + parseFloat(cs.paddingBottom);
      const chrome = modal.getBoundingClientRect().height - body.getBoundingClientRect().height;  // title, banner, buttons
      const W = innerWidth - 48 - padX - 2, H = innerHeight - 48 - chrome - padY - 2;
      const CW = Math.max(240, Math.min(420, W - 12));  // a column: 420 px, narrower in a narrow window
      body.classList.add('measuring');
      body.classList.remove('scroll');
      panels.forEach(p => body.appendChild(p));
      body.querySelectorAll('.scol').forEach(c => c.remove());
      panels.forEach(p => { p.style.width = CW + 'px'; });
      const hs = panels.map(p => p.getBoundingClientRect().height);
      // contiguous groups of the panels into n columns, the tallest column as low as possible
      const split = n => {
        const sum = (a, b) => hs.slice(a, b).reduce((x, y) => x + y, 0) + (b - a - 1) * G;
        const memo = new Map();
        const best = (i, k) => {
          const id = i + ',' + k;
          if (memo.has(id)) return memo.get(id);
          let r;
          if (k === 1) r = [sum(i, hs.length), []];
          else {
            r = [Infinity, []];
            for (let j = i + 1; j <= hs.length - k + 1; j++) {
              const rest = best(j, k - 1), m = Math.max(sum(i, j), rest[0]);
              if (m < r[0]) r = [m, [j, ...rest[1]]];
            }
          }
          memo.set(id, r);
          return r;
        };
        const r = best(0, n), bounds = [0, ...r[1], hs.length];
        return [r[0], Array.from({ length: n }, (_, c) => Array.from({ length: bounds[c + 1] - bounds[c] }, (_, k) => bounds[c] + k))];
      };
      // balanced by height (tallest first into the lowest column), each column in the panels' order
      const balance = n => {
        const cols = Array.from({ length: n }, () => ({ h: -G, idx: [] }));
        hs.map((h, i) => [h, i]).sort((x, y) => y[0] - x[0]).forEach(([h, i]) => {
          const col = cols.reduce((m, x) => (x.h < m.h ? x : m));
          col.h += h + G;
          col.idx.push(i);
        });
        cols.forEach(col => col.idx.sort((x, y) => x - y));
        cols.sort((x, y) => x.idx[0] - y.idx[0]);
        return [Math.max(...cols.map(col => col.h)), cols.map(col => col.idx)];
      };
      // as few columns as fit the height; if none do, as many as fit the width beside a vertical scroll bar (never
      // a horizontal one)
      const colsFor = w => Math.max(1, Math.min(panels.length, Math.floor((w + G) / (CW + G))));
      let pick = null;
      for (let n = 1; n <= colsFor(W); n++) {
        const a = split(n);
        if (a[0] <= H) { pick = { n, tall: a[0], groups: a[1], fits: true }; break; }
        const b = balance(n);
        if (b[0] <= H) { pick = { n, tall: b[0], groups: b[1], fits: true }; break; }
      }
      if (!pick) {
        const n = colsFor(W - 12);
        const a = split(n), b = balance(n);
        pick = { n, tall: H, groups: (a[0] <= b[0] ? a : b)[1], fits: false };
      }
      body.classList.remove('measuring');
      panels.forEach(p => { p.style.width = ''; });
      for (const g of pick.groups) {
        const col = document.createElement('div');
        col.className = 'scol';
        col.style.width = CW + 'px';
        g.forEach(i => col.appendChild(panels[i]));
        body.appendChild(col);
      }
      body.style.width = (pick.n * CW + (pick.n - 1) * G + padX + (pick.fits ? 0 : 12)) + 'px';
      body.style.height = (Math.min(pick.tall, H) + padY) + 'px';
      body.classList.toggle('scroll', !pick.fits);
      if (focused && focused !== this.shadowRoot.activeElement && body.contains(focused)) focused.focus({ preventScroll: true });
    }
    // A key binding records the next key pressed (with Shift / Ctrl / Alt). Esc cancels, Delete clears. A key another
    // action has is taken from it.
    _listenKey(btn) {
      if (this._keyDone) this._keyDone();
      btn.classList.add('listen');
      btn.textContent = 'Press a key…';
      const done = () => {
        window.removeEventListener('keydown', on, true);
        btn.classList.remove('listen');
        btn.classList.toggle('none', !+btn.dataset.code);
        btn.textContent = keyName(+btn.dataset.code);
        this._keyDone = null;
      };
      const on = e => {
        e.preventDefault();
        e.stopImmediatePropagation();
        const vk = e.keyCode;
        if (vk === 16 || vk === 17 || vk === 18 || vk === 91 || vk === 92) return;  // a modifier on its own: wait
        if (vk === 27) return done();
        const code = vk === 46 ? 0 : vk | (e.shiftKey ? 0x100 : 0) | (e.ctrlKey ? 0x200 : 0) | (e.altKey ? 0x400 : 0);
        if (code) this._over.querySelectorAll('.keyb').forEach(o => { if (o !== btn && +o.dataset.code === code) { o.dataset.code = 0; o.textContent = keyName(0); o.classList.add('none'); } });
        btn.dataset.code = code;
        done();
      };
      window.addEventListener('keydown', on, true);
      this._keyDone = done;
    }
    _settingsBanner(f) {
      const b = this._over.querySelector('[data-banner]');
      if (!b) return;
      const was = b.hidden;
      b.hidden = !f.message;
      b.innerHTML = f.message ? `<span style="flex:1">${esc(f.message)}</span>${f.restart ? key('data-sa="restart"', 'RESTART NOW', '', '', 'w') : ''}` : '';
      if (was !== b.hidden) this._settingsLayout();  // the panels' room changed
    }

    // ---- clicks
    _click(e) {
      const s = this._state || {};
      if (e.target.closest('.over')) return;  // pop-ups handle their own
      const w = e.target.closest('[data-win]');
      if (w) return this._cmd('win', { what: w.dataset.win });
      const take = e.target.closest('[data-take]');
      if (take) return this._cmd('previewTake', { slot: +take.dataset.take });
      const tsel = e.target.closest('[data-pick-tile]');
      if (tsel) return this._tileList(tsel, +tsel.dataset.pickTile);
      const mon = e.target.closest('[data-pick]');
      if (mon) return this._cmd('source', { id: mon.dataset.pick, mon: 0 });
      const u = this._unitOf(e.target), m = this._mons()[u] || {};
      const src = e.target.closest('[data-src]');
      if (src) return this._cmd('source', { id: src.dataset.src, mon: u });
      const rec = e.target.closest('[data-rec]');
      if (rec) return this._cmd('recArm', { id: rec.dataset.rec });
      const stn = e.target.closest('[data-stn]');
      if (stn) {
        const t = ((this._state || {}).stream || {}).targets || [], n = +stn.dataset.stn;
        return this._cmd('stream', { n, on: !(t[n] && t[n].state !== 'off') });
      }
      const wall = e.target.closest('[data-wall]');
      if (wall) return this._cmd('wall', { mode: wall.dataset.wall });
      const pre = e.target.closest('[data-preset]');
      if (pre) return this._cmd('preset', { index: +pre.dataset.preset, save: e.ctrlKey || e.shiftKey });
      const b = e.target.closest('[data-b]');
      if (!b || b.getAttribute('aria-disabled') === 'true') return;
      const id = b.dataset.b;
      if (id === 'manual') return this._cmd('mode', { auto: false });
      if (id === 'automode') return this._cmd('mode', { auto: true });
      if (id === 'stream') return this._cmd('stream', { on: !(s.stream && s.stream.on) });
      if (id === 'shipPrev') return this._cmd('shipStep', { dir: -1 });
      if (id === 'shipNext') return this._cmd('shipStep', { dir: 1 });
      if (id === 'shipList') return this._shipList(b);
      if (id === 'recArm') return m.id && this._cmd('recArm', { id: m.id });
      if (id === 'viewPrev' || id === 'viewNext') return m.id && this._cmd('view', { mon: u, dir: id === 'viewPrev' ? -1 : 1 });
      if (id === 'viewList') return this._viewList(b, m, u);
      if (id === 'ptzRest' || id === 'ptzAuto' || id === 'ptzFlir') { const c = this._joyCam(u); return c && this._cmd(id, { cam: c.id }); }
      if (id === 'ptzTarget') {  // TGT: the next target
        const c = this._joyCam(u), ts = (c && c.targets) || [];
        if (!ts.length) return;
        const i = ts.findIndex(x => x[0] === c.target), next = ts[(i + 1) % ts.length];
        c.target = next[0];  // shown at once; the host's next state confirms
        this._update();
        return this._cmd('ptzTarget', { cam: c.id, target: next[0] });
      }
      if (id === 'auto' || id === 'reset') return this._cmd(id, { mon: u });
      this._cmd(id);
    }

    // ---- drawing the state
    _lit(el, on, unavailable) {
      if (!el) return;
      const lamp = el.querySelector('.lamp'), c = LAMP[el.dataset.lamp];
      if (lamp && c) {
        const lit = on && !unavailable;
        lamp.style.background = c[lit ? 0 : 1];
        lamp.style.boxShadow = lit ? `0 0 6px 1px ${c[0]},inset 0 0 0 1px rgba(255,255,255,.3)` : '';
      }
      el.setAttribute('aria-pressed', on ? 'true' : 'false');
      el.setAttribute('aria-disabled', unavailable ? 'true' : 'false');
    }
    _led(el, on, colour) { if (el) el.style.background = LED[colour][on ? 0 : 1]; }
    _rocker(el, on, unavailable) {
      el.setAttribute('aria-pressed', on ? 'true' : 'false');
      el.setAttribute('aria-disabled', unavailable ? 'true' : 'false');
    }
    _text(name, v) { const e = this._t[name]; if (e && e.textContent !== v) e.textContent = v; }
    _set(el, v) { if (el && el.textContent !== v) el.textContent = v; }

    // A monitor's screen overlay: message / test bars, CCTV stand-in, crosshair, burned-in text.
    _drawScreen(ov, m, s, isProgram) {
      if (!ov) return;
      const pic = m.picture || {}, vtr = s.vtr || {}, o = n => ov.querySelector(`[data-o=${n}]`);
      ov.querySelector('.nosig').hidden = !!m.signal;
      this._set(o('msg'), m.message || 'NO SIGNAL');
      const look = !s.nativeLook && pic.cctv ? Math.min(1, (+pic.str || 0) / 200) : 0;
      ov.querySelectorAll('.look').forEach(e => { e.style.opacity = look; });
      ov.querySelector('.cross').hidden = !(m.crosshair && !s.nativeLook && m.signal);
      const vid = ov.querySelector('.vid');
      if (!s.nativeLook && pic.adj !== false) {  // web hosts: an approximation of the adjustments with CSS
        const ev = Math.pow(2, +pic.exp || 0) * (1 + (+pic.bri || 0) * 0.6) * Math.pow(1 / (+pic.gam || 1), 0.35);
        vid.style.filter = `grayscale(1) brightness(${ev.toFixed(3)}) contrast(${(+pic.con || 1).toFixed(3)})` + (look ? ` blur(${(look * 0.35).toFixed(2)}px)` : '');
      } else {
        vid.style.filter = s.nativeLook ? '' : 'grayscale(1)';
      }
      // burned-in text; a host turns it off (burnIns: false) for pictures that carry their own, like DCS's PLAT
      const burn = !!m.signal && m.burnIns !== false;
      this._set(o('time'), burn ? m.time || '' : '');
      this._set(o('ship'), burn ? m.ship || '— — —' : '');
      this._set(o('name'), burn ? (m.burnName != null ? m.burnName : m.name) || '' : '');
      const play = isProgram && vtr.play, recNow = !!vtr.rec && (m.rec !== false);
      const br = o('rec');
      this._set(br, play ? '▶ REPLAY ' + (vtr.replayTime || '') : recNow && m.signal ? '● REC' : '');
      br.style.color = play ? '#E7D04F' : '#FF6B66';
      br.classList.toggle('blink', recNow && !play);
    }

    _update() {
      const s = this._state || {}, root = this.shadowRoot, q = sel => root.querySelector(sel);
      const vtr = s.vtr || {}, bb = s.bb || {}, st = s.stream || {};
      const sources = s.sources || [], ships = s.ships || [], cams = s.cameras || [], presets = s.presets || [];
      const mons = this._mons(), wall = this._wall();
      root.querySelector('.wbx').style.display = s.windowButtons === false ? 'none' : 'flex';
      root.querySelector('[data-b=settings]').style.display = s.settings === false ? 'none' : 'flex';
      // the source keys: one per source - the deck positions as one DECK key (the view selector picks the position)
      const keyed = [];
      sources.forEach(x => {
        const id = x.group || x.id;
        if (!keyed.some(k => k.id === id)) keyed.push(x.group ? { id: x.group, name: String(x.group).toUpperCase(), plate: 'Deck cameras', key: '' } : x);
      });
      // the previews: assigned by the host (state.previews: 4 source ids, '' = none), else every source but A's
      const assigned = Array.isArray(s.previews), progId = (mons[0] || {}).id;
      const prev = assigned
        ? s.previews.slice(0, 4).map((id, k) => Object.assign({}, sources.find(x => x.id === id) || { id: '', name: '', plate: 'No source', empty: true }, { tile: k }))
        : sources.filter(x => x.id !== progId).slice(0, 4).map((x, k) => Object.assign({}, x, { tile: k }));
      while (prev.length < 4) prev.push({ tile: prev.length, id: '', name: '', plate: assigned ? 'No source' : 'No camera', key: '', empty: true });
      this._assigned = assigned;
      // lists: rebuilt only when their members change
      const sig = JSON.stringify([wall, assigned, prev.map(x => [x.id, x.name, x.plate]), keyed.map(x => [x.id, x.name, x.key]), presets.map(x => [x.name, x.key])]);
      if (sig !== this._listSig) {
        this._listSig = sig;
        const sid = p => assigned ? 'p' + p.tile : p.id || 'empty' + p.tile;  // a preview screen's id (rects, setVideo)
        const inner = p => `<div class="ovroot" data-ov="${esc(sid(p))}"${p.empty ? '' : assigned ? ` data-take="${p.tile}"` : ` data-pick="${esc(p.id)}"`}><div class="vid"></div><div class="nosig"${p.empty ? '' : ' hidden'}><div class="bars"></div><span class="burn" style="left:50%;top:50%;transform:translate(-50%,-50%);font-size:11px">${p.empty ? '' : 'NO SIGNAL'}</span></div><div class="ovl" style="opacity:.5;background:repeating-linear-gradient(0deg,rgba(0,0,0,.55) 0 1px,transparent 1px 3px)"></div><span class="burn" style="left:6px;top:4px;font-size:10px">${esc(p.name)}</span></div>`;
        const shown = wall === 'q' ? prev : [];
        if (this._native) {
          root.querySelectorAll('.top .ovroot').forEach(e => { if (!MONS.includes(e.dataset.ov)) e.remove(); });
          q('.top').insertAdjacentHTML('beforeend', shown.map(inner).join(''));
        }
        q('[data-l=prevs]').innerHTML = shown.map(p => `<div class="tile" data-tile="${p.tile}" data-sid="${esc(p.id)}"><div class="scrn" data-mon="${esc(sid(p))}">${this._native ? '' : inner(p)}</div><div class="row"><span class="led" data-ok style="background:${LED.green[1]}"></span>${assigned ? `<button class="tsel${p.empty ? ' dim' : ''}" data-pick-tile="${p.tile}" aria-label="Choose this preview's source">${esc(p.plate)} ▾</button>` : `<span class="eng sp${p.empty ? ' dim' : ''}" style="min-width:0;overflow:hidden;text-overflow:ellipsis">${esc(p.plate)}</span>`}<span class="eng dim" data-fps style="flex:none"></span><span class="eng dim" style="flex:none">${p.key ? '[' + esc(p.key) + ']' : ''}</span></div></div>`).join('');
        this._units.forEach((el, u) => {
          el.querySelector('[data-l=srcs]').innerHTML = keyed.map(b => key(`data-src="${esc(b.id)}" title="${esc(b.plate)}"`, esc(b.name), u ? '' : esc(b.key || ''), 'green')).join('') || '<span class="hint">No sources</span>';
        });
        q('[data-l=recs]').innerHTML = keyed.map(b => key(`data-rec="${esc(b.id)}" title="Record ${esc(b.plate)}"`, esc(b.name), '', 'red')).join('');
        q('[data-l=presets]').innerHTML = presets.map((b, i) => key(`data-preset="${i}" style="width:58px"`, esc(String(b.name).toUpperCase()), esc(b.key || ''), 'amber')).join('');
        Object.keys(this._videos).forEach(id => this.setVideo(id, this._videos[id]));
        this._sizes = null;  // measure again
        this._rectSig = '';
        this._dirty = true;
      }
      // title bar
      this._text('status', s.status || '');
      this._text('presetHint', s.flash || 'F5–F8 · Ctrl = save');
      // monitors and their strips
      root.querySelectorAll('[data-wall]').forEach(b => this._lit(b, b.dataset.wall === wall, false));
      this._lit(q('[data-b=swap]'), false, wall !== '2' || mons.length < 2);
      this._units.forEach((el, u) => {
        const m = mons[u] || { id: '', picture: {}, message: 'NO SIGNAL' }, pic = m.picture || {}, na = pic.unavailable || {};
        const o = n => el.querySelector(`[data-o=${n}]`);
        this._drawScreen(root.querySelector(`[data-ov=${MONS[u]}]`), m, s, u === 0);
        this._led(o('sig'), !!m.signal, 'green');
        this._set(o('plate'), m.plate || m.name || '—');
        this._set(o('res'), m.signal ? m.res || '' : '');
        o('tally').classList.toggle('on', !!vtr.rec && !!m.rec);
        const mg = (sources.find(x => x.id === m.id) || {}).group;
        el.querySelectorAll('[data-src]').forEach(b => this._lit(b, b.dataset.src === m.id || (!!mg && b.dataset.src === mg), false));
        // the camera's views (its positions on the deck)
        const views = m.views || [], vi = Math.max(0, Math.min(views.length - 1, +m.view || 0));
        el.querySelector('[data-l=view]').style.visibility = views.length > 1 ? '' : 'hidden';
        this._set(o('view'), views[vi] || '');
        this._set(o('viewN'), views.length > 1 ? `${vi + 1}/${views.length}` : '');
        this._lit(el.querySelector('[data-b=recArm]'), !!m.rec, !m.id);
        // levels
        const noLevels = !m.picture || (na.exp && na.bri && na.con && na.gam);
        el.querySelector('[data-g=lev]').classList.toggle('na', !!noLevels);
        this._set(o('group'), noLevels ? 'none' : pic.group || m.name || '');
        this._set(o('autoRange'), pic.auto && pic.autoRange ? pic.autoRange : '— · —');
        this._lit(el.querySelector('[data-b=auto]'), !!pic.auto, !m.picture || !!na.auto);
        this._lit(el.querySelector('[data-b=reset]'), false, !m.picture || !!na.reset);
        Object.keys(KNOBS).forEach(k => {
          const K = this._K(k), x = Math.max(K.min, Math.min(K.max, +pic[k] || 0)), kn = el.querySelector(`[data-knob=${k}]`);
          kn.querySelector('.ptr').setAttribute('transform', `rotate(${(-135 + this._pos(K, x) * 270).toFixed(1)})`);
          kn.classList.toggle('na', !m.picture || !!na[k]);
          const txt = K.fmt(+pic[k] || 0), val = kn.querySelector('.val');
          if (val.textContent !== txt) val.textContent = txt;
          const sv = kn.querySelector('svg');
          sv.setAttribute('aria-valuemin', K.min);
          sv.setAttribute('aria-valuemax', K.max);
          sv.setAttribute('aria-valuenow', x);
          sv.setAttribute('aria-valuetext', txt);
          sv.setAttribute('aria-disabled', na[k] ? 'true' : 'false');
        });
        // PTZ: the camera this monitor shows
        const c = m.ptz, live = !!(c && c.live);
        el.querySelector('[data-g=ptz]').classList.toggle('na', !c);
        this._set(o('cam'), c ? c.name || m.name || '' : 'fixed view');
        el.querySelector('[data-joy]').classList.toggle('na', !live);
        this._lit(el.querySelector('[data-b=ptzRest]'), false, !live);
        this._lit(el.querySelector('[data-b=ptzAuto]'), !!(c && c.auto), !live);
        const tg = c && (c.targets || []).find(x => x[0] === c.target);
        this._set(o('target'), tg ? tg[1] : '—');
        this._lit(el.querySelector('[data-b=ptzTarget]'), false, !c);
        const fl = el.querySelector('[data-b=ptzFlir]');  // experimental ([experimental] flir)
        fl.hidden = !(c && c.flirAvail);
        this._lit(fl, !!(c && c.flir), !live);
      });
      // previews
      root.querySelectorAll('.tile[data-tile]').forEach(t => {
        const id = t.dataset.sid, src = id ? sources.find(x => x.id === id) : null, signal = !!(src && src.signal);
        const ns = q(`[data-ov="${this._assigned ? 'p' + t.dataset.tile : id || 'empty' + t.dataset.tile}"] .nosig`);
        if (ns) ns.hidden = signal;
        this._led(t.querySelector('[data-ok]'), signal, 'green');
        const fps = t.querySelector('[data-fps]'), ft = src && src.fps ? Math.round(src.fps) + ' FPS' : '';
        if (fps.textContent !== ft) fps.textContent = ft;
      });
      root.querySelectorAll('[data-rec]').forEach(b => { const x = sources.find(y => y.id === b.dataset.rec || y.group === b.dataset.rec); this._lit(b, !!(x && x.rec), false); });
      // ship
      const cur = ships.find(x => x.on) || null, idx = cur ? ships.indexOf(cur) + 1 : 0;
      this._text('shipHull', cur ? cur.hull : ships.length ? '— — —' : 'NO CARRIER');
      this._text('shipUnit', cur ? cur.unit : ships.length ? 'NONE SELECTED' : '');
      this._text('shipHint', ships.length > 1 ? `${idx || '–'} of ${ships.length}` : '');
      this._lit(q('[data-b=shipPrev]'), false, ships.length < 2);
      this._lit(q('[data-b=shipNext]'), false, ships.length < 2);
      // presets
      root.querySelectorAll('[data-preset]').forEach(b => {
        const x = presets[+b.dataset.preset] || {};
        this._lit(b, !!x.active, false);
        b.style.color = x.saved ? '' : '#7C8186';
        b.title = x.saved ? 'Load ' + x.name + ' (every monitor and camera) · right-click: save, rename, clear' : 'Empty · Ctrl+' + (x.key || 'click') + ' or right-click saves';
      });
      // VTR
      const B = id => q(`[data-b=${id}]`);
      this._text('counter', vtr.counter || '00:00:00');
      this._text('vtr1', vtr.line1 || '');
      this._text('vtr2', vtr.line2 || '');
      const noVtr = vtr.ok === false;
      this._lit(B('manual'), vtr.mode === 'manual', noVtr);
      this._lit(B('automode'), vtr.mode === 'auto', noVtr);
      this._lit(B('rec'), !!vtr.rec, noVtr);
      this._lit(B('play'), !!vtr.play, noVtr);
      this._lit(B('stop'), false, noVtr);
      this._lit(B('save'), false, !vtr.canSave);
      this._lit(B('folder'), false, vtr.folder === false);
      // black box, stream
      this._rocker(B('bb'), !!bb.on, false);
      this._led(q('[data-led=bb]'), !!bb.on, 'amber');
      this._lit(B('bbSave'), false, !bb.on);
      this._text('bbInfo', bb.alert ? bb.text || '' : '');  // only what needs attention (a mishap save, no encoder)
      this._t.bbInfo.hidden = !bb.alert;
      this._t.bbInfo.style.color = bb.alert ? '#E7B04F' : '';
      this._rocker(B('stream'), !!st.on, false);
      this._led(q('[data-led=stream]'), st.state === 'live', 'red');
      q('[data-led=stream]').classList.toggle('blink', st.state === 'connecting' || st.state === 'retrying');
      const stKeys = q('[data-stn="0"]');
      if (stKeys) stKeys.parentElement.hidden = !(st.targets || []).length;  // a viewer without per-stream control
      (st.targets || []).forEach((t, n) => {
        const b = q(`[data-stn="${n}"]`);
        if (!b) return;
        this._lit(b, t.state !== 'off', !t.set);
        b.disabled = !t.set && t.state === 'off';
        const l = b.querySelector('.lamp');
        if (l) l.classList.toggle('blink', t.state === 'connecting' || t.state === 'retrying');
      });
      this._text('streamWord', st.state === 'live' ? 'LIVE' : st.on ? (st.state === 'retrying' ? 'RETRY' : 'CONNECT') : 'OFF');
      this._text('streamInfo', st.text || '');
      this._t.streamInfo.hidden = !st.text;
      this._t.streamInfo.style.color = st.state === 'retrying' ? '#E0524E' : '';
      // settings dialog
      this._settings(s.settingsForm || null);
      if (this._dirty) { this._dirty = false; this._layout(); }
    }

    // ---- layout
    // Natural sizes of the channel strip's groups and the rack's modules ([w, h], px), measured unwrapped.
    _measure() {
      const root = this.shadowRoot, st = root.querySelector('.stage');
      const u0 = this._units[0], hid = u0.style.display;
      u0.style.display = '';
      st.classList.add('measure');
      const sz = e => { const r = e.getBoundingClientRect(); return [Math.ceil(r.width) + 1, Math.ceil(r.height)]; };
      this._sizes = { groups: [...u0.querySelectorAll('.strip>.grp')].map(sz), mods: [...root.querySelectorAll('.rack>.pnl:not(.blank)')].map(sz) };
      st.classList.remove('measure');
      u0.style.display = hid;
    }

    // The solver: where the monitors, previews and rack go in a W x H stage. Every candidate arrangement is scored
    // by the picture area it gives (monitor A first; a preview counts three quarters); the best one is applied.
    _solve(W, H, scroll, z) {
      const sc = a => a.map(([w, h]) => [w * z, h * z]);
      const groups = sc(this._sizes.groups), mods = sc(this._sizes.mods), wall = this._wall();
      const stripH = inner => pack(groups, inner, SCG, SRG);
      const screenW = w => w - 2 * UB - 2 * BZ;
      const unitH = w => 2 * UB + 2 * BZ + screenW(w) * 0.75 + PL + SG + stripH(w - 2 * UB);
      const tileH = t => 2 * TB + (t - 2 * TB) * 0.75 + TP;
      const tileFromH = h => (h - 2 * TB - TP) / 0.75 + 2 * TB;
      const MINW = 300, MINT = 170;
      // the widest unit (n of them in a row or a column) that fits aw x ah
      const fitUnits = (aw, ah, n, column) => {
        let w = Math.floor(column ? aw : (aw - (n - 1) * GAP) / n);
        for (; w >= MINW; w -= 2) {
          const h = unitH(w);
          if ((column ? n * h + (n - 1) * GAP : h) <= ah) return w;
        }
        return 0;
      };
      const area = w => { const sw = screenW(w); return sw * sw * 0.75; };
      // the wall in aw x ah: { score, w, h, items: [[kind, x, y, w, ...]] }
      const solveWall = (aw, ah) => {
        if (aw < MINW || ah < 200) return null;
        if (wall === '1') {
          const w = fitUnits(aw, ah, 1, false);
          return w ? { score: area(w), w, h: unitH(w), items: [['u', 0, 0, w]] } : null;
        }
        if (wall === '2') {
          const r = fitUnits(aw, ah, 2, false), c = fitUnits(aw, ah, 2, true);
          if (!r && !c) return null;
          if (r >= c) return { score: 2 * area(r), w: 2 * r + GAP, h: unitH(r), items: [['u', 0, 0, r], ['u', r + GAP, 0, r]] };
          const h = unitH(c);
          return { score: 2 * area(c), w: c, h: 2 * h + GAP, items: [['u', 0, 0, c], ['u', 0, h + GAP, c]] };
        }
        // 1 + 4: monitor A with the previews beside it (2 x 2 or 1 x 4) or under it (4 x 1 or 2 x 2)
        let best = null;
        const top = fitUnits(aw, ah, 1, false);
        for (let w = top; w >= MINW; w -= 4) {
          const uh = unitH(w);
          if (uh > ah) continue;
          const rw = aw - w - GAP, bh = ah - uh - GAP;
          const opts = [
            ['right', 2, Math.min((rw - GAP) / 2, tileFromH((ah - GAP) / 2))],
            ['right', 1, Math.min(rw, tileFromH((ah - 3 * GAP) / 4))],
            ['below', 4, Math.min((aw - 3 * GAP) / 4, tileFromH(bh))],
            ['below', 2, Math.min((aw - GAP) / 2, tileFromH((bh - GAP) / 2))],
          ];
          for (const [where, cols, t0] of opts) {
            const t = Math.floor(t0);
            if (!(t >= MINT)) continue;
            const ts = t - 2 * TB, score = area(w) + 0.75 * 4 * ts * ts * 0.75;
            if (best && score <= best.score) continue;
            const rows = 4 / cols, pw = cols * t + (cols - 1) * GAP, ph = rows * tileH(t) + (rows - 1) * GAP;
            const items = [['u', 0, 0, w]];
            if (where === 'right') items.push(['p', w + GAP, 0, pw, cols, t]);
            else items.push(['p', 0, uh + GAP, pw, cols, t]);
            best = { score, w: where === 'right' ? w + GAP + pw : Math.max(w, pw), h: where === 'right' ? Math.max(uh, ph) : uh + GAP + ph, items };
          }
        }
        return best;
      };
      // the rack always goes under the wall (full width), whatever the window's shape (user preference)
      const cands = [];
      const iw = W - 2 * M, ih = H - 2 * M;
      const rackH = pack(mods, iw, RG, RG);
      if (isFinite(rackH)) {
        const sw = solveWall(iw, ih - rackH - GAP);
        const oneW = mods.reduce((a, m) => a + m[0], 0) + (mods.length - 1) * RG;
        // one row: blanking plates at both ends take up the slack. Even at their narrowest (padding and border) they
        // need room, or the last panel wraps onto a second row out of the window.
        const blanks = 2 * (RG + 26 * z);
        const one = rackH <= Math.max(...mods.map(m => m[1])) + 0.5 && iw >= oneW + blanks;
        // one row: no wider than the monitors (or the panels themselves, if those are wider), centred under them
        const w = one && sw ? Math.min(iw, Math.max(sw.w, oneW + blanks)) : iw;
        if (sw) cands.push({ sw, rack: { side: false, w, h: rackH, one }, bonus: 1 });
      }
      if (!cands.length) return null;
      cands.sort((a, b) => b.sw.score * b.bonus - a.sw.score * a.bonus);
      const c = cands[0], { sw, rack } = c;
      // centre the block: the wall with the rack under it, or beside it
      let wx, wy, rx, ry;
      if (rack.side) {
        const bw = sw.w + GAP + rack.w;
        wx = M + Math.max(0, (iw - bw) / 2);
        wy = M + Math.max(0, (ih - sw.h) / 2);
        rx = wx + sw.w + GAP;
        ry = wy;
      } else {
        const bh = sw.h + GAP + rack.h;
        wy = scroll ? M : M + Math.max(0, (ih - bh) / 2);
        wx = M + Math.max(0, (iw - sw.w) / 2);
        rx = M + Math.max(0, (iw - rack.w) / 2);
        ry = wy + sw.h + GAP;
      }
      return { z, score: sw.score * c.bonus, wx, wy, items: sw.items, rack: Object.assign(rack, { x: rx, y: ry }), h: rack.side ? Math.max(sw.h, rack.h) + 2 * M : sw.h + GAP + rack.h + 2 * M };
    }

    _fit() {
      const root = this.shadowRoot, stage = root.querySelector('.stage'), wall = this._wall();
      const prevs = root.querySelector('.prevs'), rack = root.querySelector('.rack');
      this._units[1].style.display = wall === '2' ? '' : 'none';
      prevs.style.display = wall === 'q' ? '' : 'none';
      if (!this._sizes) this._measure();
      const header = root.querySelector('header').getBoundingClientRect().height;
      const W = stage.clientWidth;
      let H = this.hasAttribute('fit') ? this.getBoundingClientRect().height - header : innerHeight - header;
      H = Math.max(H, 200);
      // the window too small for the layout: everything at the full width, under each other, and the page scrolls
      // Controls full size unless smaller ones buy much larger pictures (a small window): the score counts the
      // controls' scale squared.
      let sol = null, tall = 0;
      // (down to 55 %: the rack always under the monitors needs it in a 1280 x 720 window with two monitors)
      for (const z of W > 0 ? [1, 0.9, 0.8, 0.7, 0.6, 0.55] : []) {
        const x = this._solve(W, H, false, z);
        if (x && (!sol || x.score * z * z > sol.score * sol.z * sol.z)) sol = x;
      }
      if (!sol && W > 0) {
        sol = this._solve(Math.max(W, 2 * M + 320), 100000, true, 0.8);
        if (sol) tall = sol.h;
      }
      this.style.setProperty('--z', sol ? sol.z : 1);
      const wrap = root.querySelector('.wrap');
      stage.style.height = tall ? tall + 'px' : this.hasAttribute('fit') ? '' : Math.max(H, 600) + 'px';
      stage.style.flex = tall ? 'none' : '';
      wrap.style.height = tall ? (header + tall) + 'px' : '';
      if (!sol) return;
      let u = 0;
      for (const it of sol.items) {
        const x = sol.wx + it[1], y = sol.wy + it[2];
        if (it[0] === 'u') Object.assign(this._units[u++].style, { left: x + 'px', top: y + 'px', width: it[3] + 'px' });
        else Object.assign(prevs.style, { left: x + 'px', top: y + 'px', width: it[3] + 'px', gridTemplateColumns: `repeat(${it[4]}, ${it[5]}px)` });
      }
      Object.assign(rack.style, { left: sol.rack.x + 'px', top: sol.rack.y + 'px', width: sol.rack.w + 'px', justifyContent: sol.rack.side ? 'stretch' : 'center' });
      rack.classList.toggle('side', sol.rack.side);
      rack.classList.toggle('fill', !sol.rack.side && !!sol.rack.one);
    }

    // Where the monitor screens are, in CSS pixels of the viewport: the desktop host draws the pictures there.
    // Also the title bar and its buttons, so a frameless desktop window can be dragged by it.
    _layout() {
      if (!this._built) return;
      if (this.modalOpen) this._settingsLayout();
      this._fit();
      if (!this._native) return;
      const rects = {}, root = this.shadowRoot, wrap = root.querySelector('.wrap'), host = wrap.getBoundingClientRect();
      const box = r => [Math.round(r.left * 100) / 100, Math.round(r.top * 100) / 100, Math.round(r.width * 100) / 100, Math.round(r.height * 100) / 100];
      let path = `M0 0H${host.width}V${host.height}H0Z`;
      root.querySelectorAll('.scrn[data-mon]').forEach(e => {
        const r = e.getBoundingClientRect(), id = e.dataset.mon;
        const ov = root.querySelector(`.top [data-ov="${id}"]`);
        if (r.width < 4 || r.height < 4) { if (ov) ov.style.display = 'none'; return; }  // not on the wall now
        rects[id] = box(r);
        const x = r.left - host.left, y = r.top - host.top;
        path += `M${x} ${y}h${r.width}v${r.height}h${-r.width}Z`;
        if (ov) Object.assign(ov.style, { display: '', left: x + 'px', top: y + 'px', width: r.width + 'px', height: r.height + 'px' });
      });
      rects.titlebar = box(root.querySelector('header').getBoundingClientRect());
      rects.winbtns = box(root.querySelector('.winbtns').getBoundingClientRect());
      const sig = JSON.stringify(rects);
      if (sig === this._rectSig) return;
      this._rectSig = sig;
      wrap.style.clipPath = `path(evenodd, "${path}")`;
      this._cmd('layout', { rects, dpr: window.devicePixelRatio || 1 });
    }
  }

  if (!customElements.get('ilarts-console')) customElements.define('ilarts-console', IlartsConsole);
})();
