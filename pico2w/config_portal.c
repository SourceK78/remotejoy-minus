#include "config_portal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "pico/cyw43_arch.h"
#include "btstack_run_loop.h"
#include "hardware/watchdog.h"
#include "dhcpserver.h"
#include "dnsserver.h"
#include "config_store.h"
#include "bluepad_platform.h"
#include "psp_host.h"
#include "bt/uni_bt.h"

#define PORTAL_SSID "RemoteJoy-Config"
#define PORTAL_PASSWORD "remotejoy"
#define HTTP_PORT 80

static struct RjmPortalSlot g_slots[2];
static volatile int g_pairing_slot = -1;
static volatile bool g_started;
static dhcp_server_t g_dhcp_server;
static dns_server_t g_dns_server;
static struct tcp_pcb *g_http_server;
static btstack_timer_source_t g_shutdown_timer;
static bool g_shutdown_pending;
static bool g_services_initialized;
static bool g_p2_enabled = true;
static bool g_ds3_mode;
static struct RjmConfig g_mapping;
static char g_json[4096];
static char g_wifi_password[64] = PORTAL_PASSWORD;

static const char k_page[] =
"<!doctype html><html lang=ja><meta charset=utf-8><meta name=viewport content='width=device-width'>"
"<title>RemoteJoy Config</title><style>body{font-family:sans-serif;max-width:760px;margin:24px auto;padding:0 12px}"
"section{border:1px solid #bbb;border-radius:12px;padding:14px;margin:12px 0}button,select,input{padding:8px;margin:3px}"
"table{width:100%;border-collapse:collapse}td{border-top:1px solid #ddd;padding:4px}td:last-child{text-align:right}#s0,#s1{white-space:pre-line;line-height:1.7;margin-bottom:8px}"
".swatch,#colorButton{width:38px;height:38px;border:2px solid #777;border-radius:8px;padding:0}.swatch.selected{outline:3px solid #222;outline-offset:2px}"
"#colorPanel{display:none;border:1px solid #bbb;border-radius:10px;padding:8px;margin-top:6px}#rgbControls{display:none}#rgbControls label{display:block}#rgbControls input{width:70%;vertical-align:middle}"
".deadzone{margin:16px 0}.deadzone>label{display:block;margin-bottom:3px}.deadzone-control{display:flex;align-items:center;gap:8px}.deadzone-control input{flex:1;min-width:0;margin-left:0}.deadzone-control output{min-width:3em;text-align:right}</style>"
"<h1>RemoteJoy Config</h1><label>Language <select id=langSelect onchange='setLanguage(this.value)'><option value=ja>日本語</option><option value=en>English</option><option value=zh-CN>简体中文</option><option value=zh-TW>繁體中文</option><option value=ko>한국어</option><option value=es>Español</option><option value=fr>Français</option><option value=de>Deutsch</option></select></label><p id=intro></p>"
"<section><h2>1P</h2><div id=s0>---</div><button id=b0 onclick='act(0)'>---</button></section>"
"<section><h2>2P</h2><div id=s1>---</div><label><input id=p2Enabled type=checkbox onchange='setP2Enabled()'><span id=p2EnabledLabel></span></label><br><button id=b1 onclick='act(1)'>---</button></section>"
"<section><h2 id=infoTitle></h2><div><span id=btLabel></span>: <code id=btAddress>---</code></div><label><input id=ds3Mode type=checkbox onchange='setDs3Mode()'><span id=ds3ModeLabel></span></label></section>"
"<section><h2 id=wifiTitle></h2><div>SSID: <code>RemoteJoy-Config</code></div><label><span id=passwordLabel></span><input id=wifiPassword type=password minlength=8 maxlength=63 autocomplete=new-password></label><br><label><span id=confirmPasswordLabel></span><input id=wifiPasswordConfirm type=password minlength=8 maxlength=63 autocomplete=new-password></label><br><button id=wifiSaveBtn onclick='saveWifi()'></button><p id=wifiNote></p></section>"
"<section><h2 id=mapTitle></h2><select id=profile onchange='selectProfile()'></select>"
"<button id=addBtn onclick='addProfile()'></button><button id=deleteBtn onclick='deleteProfile()'></button>"
"<div><span id=ledLabel></span> <button id=colorButton onclick='toggleColorPanel()'></button>"
"<div id=colorPanel>"
"<button class=swatch data-rgb=255,0,0 style='background:#f00' title=赤 onclick='setPreset(255,0,0)'></button>"
"<button class=swatch data-rgb=0,255,0 style='background:#0f0' title=緑 onclick='setPreset(0,255,0)'></button>"
"<button class=swatch data-rgb=0,0,255 style='background:#00f' title=青 onclick='setPreset(0,0,255)'></button>"
"<button class=swatch data-rgb=255,255,0 style='background:#ff0' title=黄 onclick='setPreset(255,255,0)'></button>"
"<button class=swatch data-rgb=0,255,255 style='background:#0ff' title=シアン onclick='setPreset(0,255,255)'></button>"
"<button class=swatch data-rgb=255,0,255 style='background:#f0f' title=マゼンタ onclick='setPreset(255,0,255)'></button>"
"<button class=swatch data-rgb=255,128,0 style='background:#ff8000' title=オレンジ onclick='setPreset(255,128,0)'></button>"
"<button class=swatch data-rgb=255,255,255 style='background:#fff' title=白 onclick='setPreset(255,255,255)'></button> "
"<button id=customBtn onclick='showCustomColor()'></button>"
"<div id=rgbControls><label>R <input id=colorR type=range min=0 max=255 oninput='previewColor()' onchange='setCustomColor()'><output id=colorROut></output></label>"
"<label>G <input id=colorG type=range min=0 max=255 oninput='previewColor()' onchange='setCustomColor()'><output id=colorGOut></output></label>"
"<label>B <input id=colorB type=range min=0 max=255 oninput='previewColor()' onchange='setCustomColor()'><output id=colorBOut></output></label></div></div></div>"
"<div class=deadzone><label id=leftLabel for=leftDz></label><div class=deadzone-control><input id=leftDz type=range min=0 max=90 oninput='previewDeadzone()' onchange='setDeadzone()'><output id=leftDzOut for=leftDz></output></div></div>"
"<div class=deadzone><label id=rightLabel for=rightDz></label><div class=deadzone-control><input id=rightDz type=range min=0 max=90 oninput='previewDeadzone()' onchange='setDeadzone()'><output id=rightDzOut for=rightDz></output></div></div><div id=map></div>"
"<button id=saveBtn onclick='save()'></button><button id=exportBtn onclick='exportConfig()'></button>"
"<button id=importBtn onclick=importFile.click()></button><input id=importFile type=file accept=application/json hidden onchange='importConfig(this.files[0])'></section>"
"<button id=doneBtn onclick='done()'></button><script>let state,cfg,L,inputs,targets;"
"const texts={"
"ja:{intro:'ペアリングするスロットを選び、コントローラーをペアリングモードにしてください。',info:'本体情報',bt:'Bluetoothアドレス',mapping:'ボタンマッピング',add:'追加',del:'削除',led:'LED色',custom:'カスタム',leftDz:'左スティック デッドゾーン',rightDz:'右スティック デッドゾーン',p2Enabled:'2Pを有効にする',save:'設定を保存',exp:'エクスポート',imp:'インポート',done:'設定完了',name:'名称',address:'アドレス',status:'接続状態',searching:'検索中',connected:'接続済み',disconnected:'未接続',empty:'未登録',pair:'へペアリング',unpair:'ペアリング解除',confirm:'Pのペアリング情報を削除しますか？',max:'最大8個です',min:'最低1個必要です',saved:'保存しました',bad:'ファイル形式が正しくありません',imported:'インポートしました',doneTitle:'設定を保存しました',doneMsg:'通常モードへ戻ります。',none:'なし',rs:'右スティック',up:'上',right:'右',down:'下',left:'左'},"
"en:{intro:'Select a slot, then put the controller into pairing mode.',info:'Device information',bt:'Bluetooth address',mapping:'Button mapping',add:'Add',del:'Delete',led:'LED color',custom:'Custom',leftDz:'Left stick dead zone',rightDz:'Right stick dead zone',p2Enabled:'Enable 2P',save:'Save settings',exp:'Export',imp:'Import',done:'Finish setup',name:'Name',address:'Address',status:'Connection status',searching:'Searching',connected:'Connected',disconnected:'Disconnected',empty:'Not registered',pair:'P pair',unpair:'Unpair',confirm:'P pairing information?',max:'Maximum of 8 profiles',min:'At least one profile is required',saved:'Saved',bad:'Invalid file format',imported:'Imported',doneTitle:'Settings saved',doneMsg:'Returning to normal mode.',none:'None',rs:'Right stick',up:'Up',right:'Right',down:'Down',left:'Left'},"
"'zh-CN':{intro:'请选择配对槽位，然后将控制器置于配对模式。',info:'设备信息',bt:'蓝牙地址',mapping:'按键映射',add:'添加',del:'删除',led:'LED颜色',custom:'自定义',leftDz:'左摇杆死区',rightDz:'右摇杆死区',save:'保存设置',exp:'导出',imp:'导入',done:'完成设置',name:'名称',address:'地址',status:'连接状态',searching:'搜索中',connected:'已连接',disconnected:'未连接',empty:'未注册',pair:'P配对',unpair:'解除配对',confirm:'P的配对信息？',max:'最多8个配置',min:'至少需要一个配置',saved:'已保存',bad:'文件格式无效',imported:'已导入',doneTitle:'设置已保存',doneMsg:'正在返回普通模式。',none:'无',rs:'右摇杆',up:'上',right:'右',down:'下',left:'左'},"
"'zh-TW':{intro:'請選擇配對槽位，然後將控制器設為配對模式。',info:'裝置資訊',bt:'藍牙位址',mapping:'按鍵映射',add:'新增',del:'刪除',led:'LED顏色',custom:'自訂',leftDz:'左搖桿死區',rightDz:'右搖桿死區',save:'儲存設定',exp:'匯出',imp:'匯入',done:'完成設定',name:'名稱',address:'位址',status:'連線狀態',searching:'搜尋中',connected:'已連線',disconnected:'未連線',empty:'未註冊',pair:'P配對',unpair:'解除配對',confirm:'P的配對資訊？',max:'最多8個設定檔',min:'至少需要一個設定檔',saved:'已儲存',bad:'檔案格式無效',imported:'已匯入',doneTitle:'設定已儲存',doneMsg:'正在返回一般模式。',none:'無',rs:'右搖桿',up:'上',right:'右',down:'下',left:'左'},"
"ko:{intro:'슬롯을 선택한 다음 컨트롤러를 페어링 모드로 전환하세요.',info:'장치 정보',bt:'Bluetooth 주소',mapping:'버튼 매핑',add:'추가',del:'삭제',led:'LED 색상',custom:'사용자 지정',leftDz:'왼쪽 스틱 데드존',rightDz:'오른쪽 스틱 데드존',save:'설정 저장',exp:'내보내기',imp:'가져오기',done:'설정 완료',name:'이름',address:'주소',status:'연결 상태',searching:'검색 중',connected:'연결됨',disconnected:'연결 안 됨',empty:'등록 안 됨',pair:'P 페어링',unpair:'페어링 해제',confirm:'P 페어링 정보를 삭제할까요?',max:'최대 8개입니다',min:'최소 1개가 필요합니다',saved:'저장했습니다',bad:'파일 형식이 올바르지 않습니다',imported:'가져왔습니다',doneTitle:'설정을 저장했습니다',doneMsg:'일반 모드로 돌아갑니다.',none:'없음',rs:'오른쪽 스틱',up:'위',right:'오른쪽',down:'아래',left:'왼쪽'},"
"es:{intro:'Seleccione una ranura y ponga el mando en modo de emparejamiento.',info:'Información del dispositivo',bt:'Dirección Bluetooth',mapping:'Asignación de botones',add:'Añadir',del:'Eliminar',led:'Color LED',custom:'Personalizado',leftDz:'Zona muerta del stick izquierdo',rightDz:'Zona muerta del stick derecho',save:'Guardar ajustes',exp:'Exportar',imp:'Importar',done:'Finalizar',name:'Nombre',address:'Dirección',status:'Estado de conexión',searching:'Buscando',connected:'Conectado',disconnected:'Desconectado',empty:'Sin registrar',pair:'P emparejar',unpair:'Desemparejar',confirm:'P?',max:'Máximo 8 perfiles',min:'Se requiere al menos un perfil',saved:'Guardado',bad:'Formato de archivo no válido',imported:'Importado',doneTitle:'Ajustes guardados',doneMsg:'Volviendo al modo normal.',none:'Ninguno',rs:'Stick derecho',up:'Arriba',right:'Derecha',down:'Abajo',left:'Izquierda'},"
"fr:{intro:'Sélectionnez un emplacement, puis mettez la manette en mode appairage.',info:'Informations appareil',bt:'Adresse Bluetooth',mapping:'Mappage des boutons',add:'Ajouter',del:'Supprimer',led:'Couleur LED',custom:'Personnalisée',leftDz:'Zone morte stick gauche',rightDz:'Zone morte stick droit',save:'Enregistrer',exp:'Exporter',imp:'Importer',done:'Terminer',name:'Nom',address:'Adresse',status:'État de connexion',searching:'Recherche',connected:'Connectée',disconnected:'Déconnectée',empty:'Non enregistrée',pair:'P appairer',unpair:'Désappairer',confirm:'P ?',max:'Maximum 8 profils',min:'Au moins un profil est requis',saved:'Enregistré',bad:'Format de fichier incorrect',imported:'Importé',doneTitle:'Paramètres enregistrés',doneMsg:'Retour au mode normal.',none:'Aucun',rs:'Stick droit',up:'Haut',right:'Droite',down:'Bas',left:'Gauche'},"
"de:{intro:'Slot auswählen und den Controller in den Kopplungsmodus versetzen.',info:'Geräteinformationen',bt:'Bluetooth-Adresse',mapping:'Tastenbelegung',add:'Hinzufügen',del:'Löschen',led:'LED-Farbe',custom:'Benutzerdefiniert',leftDz:'Totzone linker Stick',rightDz:'Totzone rechter Stick',save:'Einstellungen speichern',exp:'Exportieren',imp:'Importieren',done:'Einrichtung beenden',name:'Name',address:'Adresse',status:'Verbindungsstatus',searching:'Suche',connected:'Verbunden',disconnected:'Getrennt',empty:'Nicht registriert',pair:'P koppeln',unpair:'Entkoppeln',confirm:'P?',max:'Maximal 8 Profile',min:'Mindestens ein Profil ist erforderlich',saved:'Gespeichert',bad:'Ungültiges Dateiformat',imported:'Importiert',doneTitle:'Einstellungen gespeichert',doneMsg:'Rückkehr zum Normalmodus.',none:'Keine',rs:'Rechter Stick',up:'Oben',right:'Rechts',down:'Unten',left:'Links'}};"
"const p2Labels={ja:'2Pを有効にする',en:'Enable 2P','zh-CN':'启用2P','zh-TW':'啟用2P',ko:'2P 활성화',es:'Activar 2P',fr:'Activer 2P',de:'2P aktivieren'};"
"const ds3Labels={ja:'DualShock 3モード（再起動後に反映）',en:'DualShock 3 mode (applies after restart)','zh-CN':'DualShock 3模式（重启后生效）','zh-TW':'DualShock 3模式（重新啟動後生效）',ko:'DualShock 3 모드 (재시작 후 적용)',es:'Modo DualShock 3 (tras reiniciar)',fr:'Mode DualShock 3 (après redémarrage)',de:'DualShock-3-Modus (nach Neustart)'};"
"const wifiTexts={ja:['設定モードWi-Fi','新しいパスワード','パスワード確認','パスワードを保存','次回の設定モードから反映されます。8～63文字で入力してください。','パスワードが一致しません。','パスワードを確認してください。','パスワードを保存しました。'],en:['Setup Wi-Fi','New password','Confirm password','Save password','Applied the next time setup mode starts. Enter 8–63 characters.','Passwords do not match.','Check the password.','Password saved.']};"
"function setLanguage(code){L=texts[code]||texts.en;langSelect.value=texts[code]?code:'en';localStorage.setItem('rjmLang',langSelect.value);document.documentElement.lang=langSelect.value;"
"intro.textContent=L.intro;infoTitle.textContent=L.info;btLabel.textContent=L.bt;mapTitle.textContent=L.mapping;addBtn.textContent=L.add;deleteBtn.textContent=L.del;ledLabel.textContent=L.led;customBtn.textContent=L.custom;leftLabel.textContent=L.leftDz;rightLabel.textContent=L.rightDz;p2EnabledLabel.textContent=p2Labels[langSelect.value]||p2Labels.en;ds3ModeLabel.textContent=ds3Labels[langSelect.value]||ds3Labels.en;saveBtn.textContent=L.save;exportBtn.textContent=L.exp;importBtn.textContent=L.imp;doneBtn.textContent=L.done;"
"let w=wifiTexts[langSelect.value]||wifiTexts.en;wifiTitle.textContent=w[0];passwordLabel.textContent=w[1]+' ';confirmPasswordLabel.textContent=w[2]+' ';wifiSaveBtn.textContent=w[3];wifiNote.textContent=w[4];"
"inputs=['A / Cross','B / Circle','X / Square','Y / Triangle','D-pad Up','D-pad Right','D-pad Down','D-pad Left','L1','R1','L2','R2','L3','R3','Select / Create','Start / Options','System / PS','Misc','Right stick Up','Right stick Right','Right stick Down','Right stick Left'];"
"targets=['None','SELECT','START','D-PAD UP','D-PAD RIGHT','D-PAD DOWN','D-PAD LEFT','L','R','TRIANGLE','CIRCLE','CROSS','SQUARE','HOME','VOL+','VOL-','NOTE','COMBO'];if(cfg)render();if(state)renderSlots()}"
"function initialLanguage(){let s=localStorage.getItem('rjmLang');if(s&&texts[s])return s;let n=(navigator.language||'en').toLowerCase();if(n.startsWith('zh'))return n.includes('tw')||n.includes('hk')?'zh-TW':'zh-CN';for(let x of ['ja','ko','es','fr','de'])if(n.startsWith(x))return x;return'en'}"
"async function post(path){let r=await fetch(path,{method:'POST'});if(!r.ok)throw Error(await r.text()||r.status)}"
"async function act(n){let unpair=state.slots[n].assigned;"
"if(unpair&&!confirm(L.unpair+' '+(n+1)+'P?'))return;"
"await fetch('/api/'+(unpair?'unpair/':'pair/')+n,{method:'POST'});load()}"
"function renderSlots(){btAddress.textContent=state.bt;state.slots.forEach((x,i)=>{document.getElementById('s'+i).textContent=state.pairing==i?[L.name+': ---',L.address+': ---',L.status+': '+L.searching].join(String.fromCharCode(10)):(x.assigned?[L.name+': '+x.name,L.address+': '+x.address,L.status+': '+(x.connected?L.connected:L.disconnected)].join(String.fromCharCode(10)):[L.name+': ---',L.address+': ---',L.status+': '+L.empty].join(String.fromCharCode(10)));document.getElementById('b'+i).textContent=x.assigned?L.unpair:((i+1)+L.pair)})}"
"async function load(){state=await(await fetch('/api/status')).json();renderSlots()}"
"async function loadConfig(){cfg=await(await fetch('/api/config')).json();render()}"
"async function saveWifi(){let w=wifiTexts[langSelect.value]||wifiTexts.en,p=wifiPassword.value;if(p!=wifiPasswordConfirm.value)return alert(w[5]);try{await post('/api/wifi/'+encodeURIComponent(p));wifiPassword.value=wifiPasswordConfirm.value='';alert(w[7])}catch(e){alert(w[6])}}"
"function render(){p2Enabled.checked=cfg.p2Enabled!==false;ds3Mode.checked=cfg.ds3Mode===true;profile.innerHTML=cfg.profiles.map((x,i)=>'<option value='+i+(i==cfg.active?' selected':'')+'>'+x.name+'</option>').join('');"
"let x=cfg.profiles[cfg.active];let hex='#'+x.color.map(v=>v.toString(16).padStart(2,'0')).join('');colorButton.style.background=hex;[colorR.value,colorG.value,colorB.value]=x.color;[colorROut.value,colorGOut.value,colorBOut.value]=x.color;document.querySelectorAll('.swatch').forEach(e=>e.classList.toggle('selected',e.dataset.rgb==x.color.join(',')));leftDz.value=x.deadzone[0];rightDz.value=x.deadzone[1];leftDzOut.value=x.deadzone[0]+'%';rightDzOut.value=x.deadzone[1]+'%';"
"map.innerHTML='<table>'+inputs.map((n,i)=>'<tr><td>'+n+'</td><td><select onchange=bind('+i+',this.value)>'+targets.map((t,j)=>'<option value='+j+(x.outputs[i]==j?' selected':'')+'>'+t+'</option>').join('')+'</select></td></tr>').join('')+'</table>'}"
"async function selectProfile(){await post('/api/profile/select/'+profile.value);await loadConfig()}"
"async function addProfile(){try{await post('/api/profile/add');await loadConfig()}catch(e){alert(L.max)}}"
"async function deleteProfile(){if(cfg.profiles.length==1)return alert(L.min);await post('/api/profile/delete/'+cfg.active);await loadConfig()}"
"async function setRgb(r,g,b){await post('/api/profile/color/'+cfg.active+'/'+r+'/'+g+'/'+b);await loadConfig()}"
"function toggleColorPanel(){colorPanel.style.display=colorPanel.style.display=='block'?'none':'block'}"
"function showCustomColor(){rgbControls.style.display='block'}"
"async function setPreset(r,g,b){rgbControls.style.display='none';colorPanel.style.display='none';await setRgb(r,g,b)}"
"function previewColor(){colorROut.value=colorR.value;colorGOut.value=colorG.value;colorBOut.value=colorB.value;colorButton.style.background='rgb('+colorR.value+','+colorG.value+','+colorB.value+')'}"
"async function setCustomColor(){previewColor();await setRgb(colorR.value,colorG.value,colorB.value)}"
"function previewDeadzone(){leftDzOut.value=leftDz.value+'%';rightDzOut.value=rightDz.value+'%'}"
"async function setDeadzone(){await post('/api/profile/deadzone/'+cfg.active+'/'+leftDz.value+'/'+rightDz.value);cfg.profiles[cfg.active].deadzone=[+leftDz.value,+rightDz.value];render()}"
"async function bind(i,v){await post('/api/profile/bind/'+cfg.active+'/'+i+'/'+v);cfg.profiles[cfg.active].outputs[i]=+v}"
"async function setP2Enabled(){await post('/api/p2/enabled/'+(p2Enabled.checked?1:0));cfg.p2Enabled=p2Enabled.checked}"
"async function setDs3Mode(){await post('/api/ds3/enabled/'+(ds3Mode.checked?1:0));cfg.ds3Mode=ds3Mode.checked}"
"async function save(){await post('/api/config/save');alert(L.saved)}"
"function exportConfig(){let a=document.createElement('a');a.href=URL.createObjectURL(new Blob([JSON.stringify(cfg,null,2)],{type:'application/json'}));a.download='remotejoy-mapping.json';a.click();URL.revokeObjectURL(a.href)}"
"async function importConfig(f){try{let c=JSON.parse(await f.text());if(c.version!=2||!Array.isArray(c.profiles)||!c.profiles.length||c.profiles.length>8)throw 0;"
"for(let x of c.profiles)if(!Array.isArray(x.color)||x.color.length!=3||!Array.isArray(x.deadzone)||x.deadzone.length!=2||!Array.isArray(x.outputs)||x.outputs.length!=inputs.length||x.color.some(v=>v<0||v>255)||x.deadzone.some(v=>v<0||v>90)||x.outputs.some(v=>v<0||v>=targets.length))throw 0;"
"await post('/api/profile/reset');for(let i=1;i<c.profiles.length;i++)await post('/api/profile/add');"
"for(let i=0;i<c.profiles.length;i++){let x=c.profiles[i];await post('/api/profile/color/'+i+'/'+x.color.join('/'));await post('/api/profile/deadzone/'+i+'/'+x.deadzone.join('/'));for(let t=0;t<inputs.length;t++)await post('/api/profile/bind/'+i+'/'+t+'/'+x.outputs[t])}"
"await post('/api/profile/select/'+Math.min(c.active||0,c.profiles.length-1));await post('/api/p2/enabled/'+(c.p2Enabled===false?0:1));await post('/api/ds3/enabled/'+(c.ds3Mode===true?1:0));await post('/api/config/save');await loadConfig();alert(L.imported)}catch(e){alert(L.bad)}}"
"async function done(){await post('/api/done');document.body.innerHTML='<h1>'+L.doneTitle+'</h1><p>'+L.doneMsg+'</p>'}"
"setLanguage(initialLanguage());load();loadConfig();setInterval(load,1000)</script></html>";

static void shutdown_timer_handler(btstack_timer_source_t *timer)
{
    (void)timer;
    /* CYW43 AP disable/re-enable is unreliable while Bluetooth remains active
       with Pico SDK 2.1. Reboot into normal mode so both stacks start cleanly.
       Slot assignments and Bluetooth keys have already been persisted. */
    watchdog_reboot(0, 0, 0);
}

static void schedule_portal_shutdown(void)
{
    if (g_shutdown_pending) return;
    g_shutdown_pending = true;
    rjm_bluepad_prepare_reboot();
    btstack_run_loop_set_timer_handler(&g_shutdown_timer, shutdown_timer_handler);
    /* Allow the Bluetooth disconnect packets and the HTTP response to leave
       before resetting CYW43.  The watchdog remains the final fallback even
       if a controller does not acknowledge disconnection. */
    btstack_run_loop_set_timer(&g_shutdown_timer, 1500);
    btstack_run_loop_add_timer(&g_shutdown_timer);
}

static void send_response(struct tcp_pcb *pcb, const char *status, const char *type,
                          const char *body, size_t length)
{
    char header[160];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
                     status, type, (unsigned)length);
    tcp_write(pcb, header, (u16_t)n, TCP_WRITE_FLAG_COPY);
    tcp_write(pcb, body, (u16_t)length, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
}

static void send_portal_redirect(struct tcp_pcb *pcb)
{
    static const char response[] =
        "HTTP/1.1 302 Found\r\n"
        "Location: http://192.168.4.1/\r\n"
        "Cache-Control: no-store\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n";
    tcp_write(pcb, response, sizeof(response) - 1, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
}

struct HttpStream {
    const char *body;
    size_t length;
    size_t offset;
};

static void stream_error(void *arg, err_t err)
{
    (void)err;
    free(arg);
}

static err_t stream_sent(void *arg, struct tcp_pcb *pcb, u16_t acknowledged)
{
    struct HttpStream *stream = arg;
    (void)acknowledged;
    if (!stream) return ERR_OK;
    if (stream->offset < stream->length) {
        u16_t available = tcp_sndbuf(pcb);
        u16_t chunk = (u16_t)(stream->length - stream->offset);
        if (chunk > 1400) chunk = 1400;
        if (chunk > available) chunk = available;
        if (chunk && tcp_write(pcb, stream->body + stream->offset, chunk, TCP_WRITE_FLAG_COPY) == ERR_OK) {
            stream->offset += chunk;
            tcp_output(pcb);
        }
        return ERR_OK;
    }
    tcp_arg(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_err(pcb, NULL);
    free(stream);
    tcp_close(pcb);
    return ERR_OK;
}

static bool send_stream_response(struct tcp_pcb *pcb, const char *body, size_t length)
{
    char header[160];
    struct HttpStream *stream = calloc(1, sizeof(*stream));
    if (!stream) return false;
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: %u\r\nConnection: close\r\n\r\n", (unsigned)length);
    stream->body = body;
    stream->length = length;
    tcp_arg(pcb, stream);
    tcp_sent(pcb, stream_sent);
    tcp_err(pcb, stream_error);
    if (tcp_write(pcb, header, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        tcp_arg(pcb, NULL); tcp_sent(pcb, NULL); tcp_err(pcb, NULL); free(stream); return false;
    }
    stream_sent(stream, pcb, 0);
    return true;
}

static void make_status(char *out, size_t size)
{
    char slot[2][160];
    bd_addr_t local_addr;
    uni_bt_get_local_bd_addr_safe(local_addr);
    for (int i = 0; i < 2; ++i) {
        const struct RjmPortalSlot *s = &g_slots[i];
        snprintf(slot[i], sizeof(slot[i]),
                 "{\"assigned\":%s,\"connected\":%s,\"name\":\"%.39s\","
                 "\"address\":\"%02X:%02X:%02X:%02X:%02X:%02X\"}",
                 s->assigned ? "true" : "false", s->connected ? "true" : "false", s->name,
                 s->address[0], s->address[1], s->address[2], s->address[3], s->address[4], s->address[5]);
    }
    snprintf(out, size, "{\"pairing\":%d,\"psp\":\"%s\","
             "\"bt\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
             "\"slots\":[%s,%s]}",
             g_pairing_slot, rjm_psp_host_status(), local_addr[0], local_addr[1], local_addr[2],
             local_addr[3], local_addr[4], local_addr[5], slot[0], slot[1]);
}

static void make_config(char *out, size_t size)
{
    size_t used = 0;
#define APPEND(...) do { if (used < size) { int n = snprintf(out + used, size - used, __VA_ARGS__); if (n > 0) used += (size_t)n; } } while (0)
    APPEND("{\"version\":%u,\"active\":%u,\"p2Enabled\":%s,\"ds3Mode\":%s,\"profiles\":[",
           (unsigned)g_mapping.version, (unsigned)g_mapping.active_profile,
           g_p2_enabled ? "true" : "false", g_ds3_mode ? "true" : "false");
    for (uint8_t p = 0; p < g_mapping.profile_count; ++p) {
        const struct RjmMappingProfile *profile = &g_mapping.profile[p];
        APPEND("%s{\"name\":\"%.23s\",\"color\":[%u,%u,%u],\"deadzone\":[%u,%u],\"outputs\":[",
               p ? "," : "", profile->name, profile->color.red, profile->color.green, profile->color.blue,
               profile->left_deadzone_percent, profile->right_deadzone_percent);
        for (uint8_t b = 0; b < RJM_MAPPABLE_INPUT_COUNT; ++b)
            APPEND("%s%u", b ? "," : "", profile->output[b]);
        APPEND("]}");
    }
    APPEND("]}");
#undef APPEND
}

static void reset_mapping(void)
{
    rjm_config_set_defaults(&g_mapping);
}

static bool add_profile(void)
{
    if (g_mapping.profile_count >= RJM_PROFILE_MAX) return false;
    uint8_t index = g_mapping.profile_count++;
    g_mapping.profile[index] = g_mapping.profile[g_mapping.active_profile];
    snprintf(g_mapping.profile[index].name, sizeof(g_mapping.profile[index].name),
             "Profile %u", (unsigned)(index + 1));
    g_mapping.active_profile = index;
    return true;
}

static bool delete_profile(unsigned index)
{
    if (g_mapping.profile_count <= 1 || index >= g_mapping.profile_count) return false;
    memmove(&g_mapping.profile[index], &g_mapping.profile[index + 1],
            (g_mapping.profile_count - index - 1) * sizeof(g_mapping.profile[0]));
    --g_mapping.profile_count;
    if (g_mapping.active_profile >= g_mapping.profile_count)
        g_mapping.active_profile = g_mapping.profile_count - 1;
    return true;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool decode_url_component(char *out, size_t out_size, const char *start, size_t length)
{
    size_t used = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char value = (unsigned char)start[i];
        if (value == '%' && i + 2 < length) {
            int hi = hex_digit(start[i + 1]), lo = hex_digit(start[i + 2]);
            if (hi < 0 || lo < 0) return false;
            value = (unsigned char)((hi << 4) | lo);
            i += 2;
        }
        if (value == 0 || used + 1 >= out_size) return false;
        out[used++] = (char)value;
    }
    out[used] = 0;
    return true;
}

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    char request[384] = {0};
    char json[1024];
    unsigned a, b, c, d;
    (void)arg;
    if (!p) {
        if (arg) { free(arg); tcp_arg(pcb, NULL); tcp_sent(pcb, NULL); tcp_err(pcb, NULL); }
        tcp_close(pcb);
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }
    pbuf_copy_partial(p, request, p->tot_len < sizeof(request) - 1 ? p->tot_len : sizeof(request) - 1, 0);
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    if (strncmp(request, "POST /api/pair/0 ", 17) == 0) {
        g_pairing_slot = 0;
        rjm_bluepad_start_pairing();
        send_response(pcb, "204 No Content", "text/plain", "", 0);
    } else if (strncmp(request, "POST /api/pair/1 ", 17) == 0) {
        g_pairing_slot = 1;
        rjm_bluepad_start_pairing();
        send_response(pcb, "204 No Content", "text/plain", "", 0);
    } else if (strncmp(request, "POST /api/unpair/0 ", 19) == 0) {
        send_response(pcb, rjm_bluepad_request_unpair(0) ? "204 No Content" : "409 Conflict",
                      "text/plain", "", 0);
    } else if (strncmp(request, "POST /api/unpair/1 ", 19) == 0) {
        send_response(pcb, rjm_bluepad_request_unpair(1) ? "204 No Content" : "409 Conflict",
                      "text/plain", "", 0);
    } else if (strncmp(request, "POST /api/done ", 15) == 0) {
        rjm_config_store_save_mapping(&g_mapping);
        rjm_config_store_save_p2_enabled(g_p2_enabled);
        rjm_config_store_save_ds3_mode(g_ds3_mode);
        send_response(pcb, "202 Accepted", "text/plain", "", 0);
        schedule_portal_shutdown();
    } else if (strncmp(request, "GET /api/config ", 16) == 0) {
        make_config(g_json, sizeof(g_json));
        send_response(pcb, "200 OK", "application/json", g_json, strlen(g_json));
    } else if (strncmp(request, "POST /api/config/save ", 22) == 0) {
        bool ok = rjm_config_store_save_mapping(&g_mapping) &&
                  rjm_config_store_save_p2_enabled(g_p2_enabled) &&
                  rjm_config_store_save_ds3_mode(g_ds3_mode);
        send_response(pcb, ok ? "204 No Content" : "500 Internal Server Error",
                      "text/plain", "", 0);
    } else if (sscanf(request, "POST /api/p2/enabled/%u ", &a) == 1) {
        bool ok = a <= 1;
        if (ok) {
            g_p2_enabled = a != 0;
            rjm_bluepad_update_scan_state();
        }
        send_response(pcb, ok ? "204 No Content" : "400 Bad Request", "text/plain", "", 0);
    } else if (sscanf(request, "POST /api/ds3/enabled/%u ", &a) == 1) {
        bool ok = a <= 1;
        if (ok) {
            g_ds3_mode = a != 0;
            ok = rjm_config_store_save_ds3_mode(g_ds3_mode);
        }
        send_response(pcb, ok ? "204 No Content" : "400 Bad Request", "text/plain", "", 0);
    } else if (strncmp(request, "POST /api/wifi/", 15) == 0) {
        char password[64];
        char *start = request + 15;
        char *space = strchr(start, ' ');
        bool ok = space &&
                  decode_url_component(password, sizeof(password), start, (size_t)(space - start)) &&
                  rjm_config_store_save_wifi_password(password);
        if (ok)
            snprintf(g_wifi_password, sizeof(g_wifi_password), "%s", password);
        send_response(pcb, ok ? "204 No Content" : "400 Bad Request", "text/plain", "", 0);
    } else if (strncmp(request, "POST /api/profile/reset ", 24) == 0) {
        reset_mapping();
        send_response(pcb, "204 No Content", "text/plain", "", 0);
    } else if (strncmp(request, "POST /api/profile/add ", 22) == 0) {
        send_response(pcb, add_profile() ? "204 No Content" : "409 Conflict", "text/plain", "", 0);
    } else if (sscanf(request, "POST /api/profile/delete/%u ", &a) == 1) {
        send_response(pcb, delete_profile(a) ? "204 No Content" : "409 Conflict", "text/plain", "", 0);
    } else if (sscanf(request, "POST /api/profile/select/%u ", &a) == 1) {
        bool ok = a < g_mapping.profile_count;
        if (ok) g_mapping.active_profile = (uint8_t)a;
        send_response(pcb, ok ? "204 No Content" : "400 Bad Request", "text/plain", "", 0);
    } else if (sscanf(request, "POST /api/profile/color/%u/%u/%u/%u ", &a, &b, &c, &d) == 4) {
        bool ok = a < g_mapping.profile_count && b <= 255 && c <= 255 && d <= 255;
        if (ok) {
            g_mapping.profile[a].color = (struct RjmRgb){(uint8_t)b, (uint8_t)c, (uint8_t)d};
        }
        send_response(pcb, ok ? "204 No Content" : "400 Bad Request", "text/plain", "", 0);
    } else if (sscanf(request, "POST /api/profile/deadzone/%u/%u/%u ", &a, &b, &c) == 3) {
        bool ok = a < g_mapping.profile_count && b <= 90 && c <= 90;
        if (ok) {
            g_mapping.profile[a].left_deadzone_percent = (uint8_t)b;
            g_mapping.profile[a].right_deadzone_percent = (uint8_t)c;
        }
        send_response(pcb, ok ? "204 No Content" : "400 Bad Request", "text/plain", "", 0);
    } else if (sscanf(request, "POST /api/profile/bind/%u/%u/%u ", &a, &b, &c) == 3) {
        bool ok = a < g_mapping.profile_count && b < RJM_MAPPABLE_INPUT_COUNT && c < RJM_OUTPUT_COUNT;
        if (ok) g_mapping.profile[a].output[b] = (uint8_t)c;
        send_response(pcb, ok ? "204 No Content" : "400 Bad Request", "text/plain", "", 0);
    } else if (strncmp(request, "GET /api/status ", 16) == 0) {
        make_status(json, sizeof(json));
        send_response(pcb, "200 OK", "application/json", json, strlen(json));
    } else if (strncmp(request, "GET /generate_204 ", 18) == 0 ||
               strncmp(request, "GET /gen_204 ", 13) == 0 ||
               strncmp(request, "GET /hotspot-detect.html ", 25) == 0 ||
               strncmp(request, "GET /connecttest.txt ", 21) == 0 ||
               strncmp(request, "GET /ncsi.txt ", 14) == 0) {
        send_portal_redirect(pcb);
    } else if (strncmp(request, "GET ", 4) == 0) {
        /* Captive portal fallback for Android and other clients. */
        if (send_stream_response(pcb, k_page, sizeof(k_page) - 1)) return ERR_OK;
        send_response(pcb, "500 Internal Server Error", "text/plain", "No memory", 9);
    } else {
        send_response(pcb, "404 Not Found", "text/plain", "Not found", 9);
    }
    tcp_close(pcb);
    return ERR_OK;
}

static err_t http_accept(void *arg, struct tcp_pcb *client, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !client) return ERR_VAL;
    tcp_recv(client, http_recv);
    return ERR_OK;
}

bool rjm_portal_start(void)
{
    struct tcp_pcb *server;
    ip4_addr_t gateway;
    ip4_addr_t mask;
    if (g_started) return true;
    /* Configuration mode owns controller input; release PSP controls first. */
    rjm_psp_host_set_enabled(false);
    if (!rjm_config_store_load_wifi_password(g_wifi_password))
        snprintf(g_wifi_password, sizeof(g_wifi_password), "%s", PORTAL_PASSWORD);
    cyw43_arch_enable_ap_mode(PORTAL_SSID, g_wifi_password, CYW43_AUTH_WPA2_AES_PSK);
    if (g_services_initialized) {
        g_started = true;
        return true;
    }
    gateway.addr = PP_HTONL(CYW43_DEFAULT_IP_AP_ADDRESS);
    mask.addr = PP_HTONL(CYW43_DEFAULT_IP_MASK);
    cyw43_arch_lwip_begin();
    dhcp_server_init(&g_dhcp_server, &cyw43_state.netif[CYW43_ITF_AP], &gateway, &mask);
    dns_server_init(&g_dns_server, &cyw43_state.netif[CYW43_ITF_AP], &gateway);
    server = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!server || tcp_bind(server, IP_ANY_TYPE, HTTP_PORT) != ERR_OK) {
        cyw43_arch_lwip_end();
        return false;
    }
    server = tcp_listen_with_backlog(server, 4);
    if (!server) {
        cyw43_arch_lwip_end();
        return false;
    }
    g_http_server = server;
    tcp_accept(server, http_accept);
    cyw43_arch_lwip_end();
    g_services_initialized = true;
    g_started = true;
    return true;
}

bool rjm_portal_is_started(void) { return g_started; }

bool rjm_portal_reset_wifi(void)
{
    return rjm_config_store_reset_wifi();
}

int rjm_portal_pairing_slot(void) { return g_pairing_slot; }

void rjm_portal_complete_pairing(int slot, const uint8_t address[RJM_BT_ADDR_LEN], const char *name)
{
    static const uint8_t zero_address[RJM_BT_ADDR_LEN] = {0};
    if (slot < 0 || slot >= 2) return;
    /* Never persist an incomplete/virtual Bluepad32 device. */
    if (!address || memcmp(address, zero_address, sizeof(zero_address)) == 0) return;
    /* A controller address belongs to exactly one player slot. */
    for (int i = 0; i < 2; ++i) {
        if (i != slot && g_slots[i].assigned &&
            memcmp(g_slots[i].address, address, RJM_BT_ADDR_LEN) == 0)
            memset(&g_slots[i], 0, sizeof(g_slots[i]));
    }
    g_slots[slot].assigned = true;
    /* Mark connected only after the first valid controller input report. */
    g_slots[slot].connected = false;
    memcpy(g_slots[slot].address, address, RJM_BT_ADDR_LEN);
    snprintf(g_slots[slot].name, sizeof(g_slots[slot].name), "%s",
             (name && name[0]) ? name : "Controller");
    if (slot == 1) {
        g_p2_enabled = true;
        rjm_config_store_save_p2_enabled(true);
    }
    g_pairing_slot = -1;
    rjm_config_store_save_slots(g_slots);
}

void rjm_portal_restore_slots(const struct RjmPortalSlot slots[2])
{
    memcpy(g_slots, slots, sizeof(g_slots));
    /* Repair legacy duplicate registrations deterministically: 1P wins. */
    if (g_slots[0].assigned && g_slots[1].assigned &&
        memcmp(g_slots[0].address, g_slots[1].address, RJM_BT_ADDR_LEN) == 0) {
        memset(&g_slots[1], 0, sizeof(g_slots[1]));
        rjm_config_store_save_slots(g_slots);
    }
}

void rjm_portal_get_slots(struct RjmPortalSlot slots[2])
{
    memcpy(slots, g_slots, sizeof(g_slots));
}

void rjm_portal_complete_unpair(const uint8_t address[RJM_BT_ADDR_LEN])
{
    for (int i = 0; i < 2; ++i) {
        if (g_slots[i].assigned && memcmp(g_slots[i].address, address, RJM_BT_ADDR_LEN) == 0)
            memset(&g_slots[i], 0, sizeof(g_slots[i]));
    }
    rjm_config_store_save_slots(g_slots);
}

void rjm_portal_set_connected(const uint8_t address[RJM_BT_ADDR_LEN], bool connected)
{
    for (int i = 0; i < 2; ++i) {
        if (g_slots[i].assigned && memcmp(g_slots[i].address, address, RJM_BT_ADDR_LEN) == 0)
            g_slots[i].connected = connected;
    }
}

void rjm_portal_restore_mapping(const struct RjmConfig *config)
{
    if (rjm_config_validate(config)) {
        g_mapping = *config;
    } else reset_mapping();
}

struct RjmRgb rjm_portal_active_color(void)
{
    if (!rjm_config_validate(&g_mapping)) reset_mapping();
    return g_mapping.profile[g_mapping.active_profile].color;
}

void rjm_portal_apply_active_mapping(const struct RjmNormalizedState *input,
                                     struct RjmMappedState *output)
{
    if (!rjm_config_validate(&g_mapping)) reset_mapping();
    rjm_apply_profile(&g_mapping.profile[g_mapping.active_profile], input, output);
}

void rjm_portal_next_profile(void)
{
    if (!rjm_config_validate(&g_mapping)) reset_mapping();
    g_mapping.active_profile = (uint8_t)((g_mapping.active_profile + 1) % g_mapping.profile_count);
    rjm_config_store_save_mapping(&g_mapping);
}

bool rjm_portal_p2_enabled(void) { return g_p2_enabled; }

void rjm_portal_restore_p2_enabled(bool enabled) { g_p2_enabled = enabled; }
bool rjm_portal_ds3_mode(void) { return g_ds3_mode; }
void rjm_portal_restore_ds3_mode(bool enabled) { g_ds3_mode = enabled; }
