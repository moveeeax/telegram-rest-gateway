// HTML страниц онбординга. Без внешних ресурсов: стили и скрипт inline, скрипт под nonce из CSP.
const esc = (s: string) => s.replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]!);

const STYLE = `
:root{--bg:#f6f7f9;--fg:#15181d;--muted:#5b6370;--card:#fff;--line:#dde1e6;--accent:#2a6fdb}
@media (prefers-color-scheme:dark){:root{--bg:#111418;--fg:#e8ebef;--muted:#9aa3ae;--card:#1a1e24;--line:#2c323a;--accent:#6ea2ff}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 system-ui,-apple-system,sans-serif}
main{max-width:460px;margin:0 auto;padding:32px 16px}h1{font-size:22px;margin:0 0 16px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:20px}
.muted{color:var(--muted);font-size:14px}#qr svg{width:100%;max-width:280px;height:auto;display:block;margin:12px auto;background:#fff;border-radius:8px}
input{width:100%;padding:10px;font:inherit;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--fg);margin:8px 0}
button{padding:10px 14px;font:inherit;border:0;border-radius:8px;background:var(--accent);color:#fff;cursor:pointer}
button.link{background:none;color:var(--accent);padding:0;margin-top:12px}
code,pre{font:13px/1.4 ui-monospace,monospace;word-break:break-all;white-space:pre-wrap}pre{background:var(--bg);border:1px solid var(--line);border-radius:8px;padding:10px}
.err{color:#c0392b;margin-top:8px}`;

function shell(title: string, body: string, nonce?: string, script?: string): string {
  return `<!doctype html><html lang="ru"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>${esc(title)}</title><style>${STYLE}</style></head><body><main>${body}</main>${script && nonce ? `<script nonce="${nonce}">${script}</script>` : ""}</body></html>`;
}

export function messagePage(message: string): string {
  return shell("Подключение Telegram", `<h1>Подключение Telegram</h1><div class="card"><p>${esc(message)}</p></div>`);
}

export function tokenPage(token: string, apiUrl: string): string {
  return shell("Новый токен", `<h1>Новый токен</h1><div class="card">
<p>Токен показывается один раз. Сохраните его, старый перестанет работать через минуту.</p>
<pre>${esc(token)}</pre><p class="muted">API: ${esc(apiUrl)}, заголовок <code>Authorization: Bearer &lt;токен&gt;</code></p></div>`);
}

const ONBOARD_SCRIPT = `
const $=(id)=>document.getElementById(id);let phoneMode=false,busy=false;
async function post(path,value){busy=true;try{const r=await fetch(path,{method:"POST",headers:{"content-type":"application/json"},body:JSON.stringify({value})});render(await r.json());}finally{busy=false;}}
function show(id){for(const s of ["starting","qr","phone","code","password","ready","final"])$(s).hidden=s!==id;}
function render(v){
  $("err").textContent=v.error||"";
  if(v.phase==="starting"){show("starting");return;}
  if(v.phase==="qr"){if(phoneMode){show("phone");return;}show("qr");if(v.qr_svg)$("qr-img").innerHTML=v.qr_svg;return;}
  if(v.phase==="code"){show("code");const ci=v.code_info;$("code-hint").textContent=ci?("Код отправлен ("+ci.type+"), "+ci.length+" цифр"):"";return;}
  if(v.phase==="password"){show("password");return;}
  if(v.phase==="ready"){show("ready");stop();
    if(v.token){$("token").textContent=v.token;$("api").textContent=v.api_url;$("with-token").hidden=false;
      if(v.mcp_url){$("mcp").hidden=false;$("mcp-url").textContent=v.mcp_url;$("mcp-add").textContent=v.claude_mcp_add;}}
    else{$("no-token").hidden=false;}return;}
  show("final");stop();
  $("final-text").textContent=v.phase==="duplicate"?"этот аккаунт уже подключен":v.phase==="expired"?"Время подключения истекло. Откройте ссылку еще раз.":(v.error||"Ошибка подключения.");
}
let timer=setInterval(poll,2000);function stop(){clearInterval(timer);}
async function poll(){if(busy)return;try{render(await (await fetch("/onboard/state")).json());}catch(e){}}
$("to-phone").onclick=()=>{phoneMode=true;show("phone");};
$("phone-form").onsubmit=(e)=>{e.preventDefault();phoneMode=false;post("/onboard/phone",$("phone-in").value);};
$("code-form").onsubmit=(e)=>{e.preventDefault();post("/onboard/code",$("code-in").value);};
$("pw-form").onsubmit=(e)=>{e.preventDefault();post("/onboard/password",$("pw-in").value);};
poll();`;

export function onboardPage(nonce: string): string {
  const body = `<h1>Подключение Telegram</h1><div class="card">
<section id="starting"><p>запускаем…</p><p class="muted">Обычно это занимает до минуты.</p></section>
<section id="qr" hidden><p>Отсканируйте код в приложении Telegram на телефоне:</p><p><b>Telegram → Настройки → Устройства → Подключить устройство</b></p>
<div id="qr-img"></div><button class="link" id="to-phone" type="button">по номеру телефона</button></section>
<section id="phone" hidden><form id="phone-form"><label>Номер телефона<input id="phone-in" type="tel" autocomplete="tel" placeholder="+7…" required></label><button>Получить код</button></form></section>
<section id="code" hidden><form id="code-form"><label>Код из Telegram<input id="code-in" inputmode="numeric" autocomplete="one-time-code" required></label><p class="muted" id="code-hint"></p><button>Войти</button></form></section>
<section id="password" hidden><form id="pw-form"><label>Облачный пароль (двухэтапная проверка)<input id="pw-in" type="password" autocomplete="current-password" required></label><button>Войти</button></form></section>
<section id="ready" hidden><p>Аккаунт подключен.</p>
<div id="with-token" hidden><p>Ваш токен. Он показывается один раз, сохраните его:</p><pre id="token"></pre>
<p class="muted">API: <span id="api"></span>, заголовок <code>Authorization: Bearer &lt;токен&gt;</code></p>
<div id="mcp" hidden><p>MCP для Claude: <code id="mcp-url"></code>, заголовок <code>X-API-Key</code> с тем же токеном.</p><pre id="mcp-add"></pre></div></div>
<p id="no-token" hidden>Токен прежний, менять ничего не нужно.</p></section>
<section id="final" hidden><p id="final-text"></p></section>
<p class="err" id="err" role="alert"></p></div>`;
  return shell("Подключение Telegram", body, nonce, ONBOARD_SCRIPT);
}
