/*
 * config_portal.c - Web configuration mode for the RP2040-zero build.
 *
 * The native USB port enumerates as a CDC-NCM network adapter. lwIP runs
 * polled (NO_SYS) with a small DHCP server, and the page and JSON API follow
 * the Pico 2 W portal with the pairing, Wi-Fi, gyro and 2P controls removed.
 */
#include "config_portal.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/board_api.h"
#include "dhserver.h"
#include "hardware/watchdog.h"
#include "lwip/init.h"
#include "lwip/ip.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "mapping_config.h"

#define HTTP_PORT 80
#define REBOOT_DELAY_MS 1000u
#define LED_BREATH_PERIOD_MS 2000u

#define INIT_IP4(a, b, c, d) { PP_HTONL(LWIP_MAKEU32(a, b, c, d)) }

/* Locally administered address; lwIP uses the same with the last bit flipped. */
uint8_t tud_network_mac_address[6] = {0x02, 0x52, 0x4A, 0x4D, 0x00, 0x00};

static struct netif g_netif;
static struct pbuf *g_received_frame;
static char g_json[4096];
static bool g_reboot_pending;
static uint32_t g_reboot_at_ms;

static const ip4_addr_t k_ipaddr = INIT_IP4(192, 168, 7, 1);
static const ip4_addr_t k_netmask = INIT_IP4(255, 255, 255, 0);
static const ip4_addr_t k_gateway = INIT_IP4(0, 0, 0, 0);

static dhcp_entry_t g_dhcp_entries[] = {
    {{0}, INIT_IP4(192, 168, 7, 2), 24 * 60 * 60},
    {{0}, INIT_IP4(192, 168, 7, 3), 24 * 60 * 60},
    {{0}, INIT_IP4(192, 168, 7, 4), 24 * 60 * 60},
};

static const dhcp_config_t k_dhcp_config = {
    .router = INIT_IP4(0, 0, 0, 0),
    .port = 67,
    .dns = INIT_IP4(0, 0, 0, 0),
    .domain = "usb",
    .num_entry = TU_ARRAY_SIZE(g_dhcp_entries),
    .entries = g_dhcp_entries,
};

static const char k_page[] =
"<!doctype html><html lang=ja><meta charset=utf-8><meta name=viewport content='width=device-width'>"
"<title>RemoteJoy Config</title><style>body{font-family:sans-serif;max-width:760px;margin:24px auto;padding:0 12px}"
"section{border:1px solid #bbb;border-radius:12px;padding:14px;margin:12px 0}button,select,input{padding:8px;margin:3px}"
"table{width:100%;border-collapse:collapse}td{border-top:1px solid #ddd;padding:4px}td:last-child{text-align:right}"
".swatch,#colorButton{width:38px;height:38px;border:2px solid #777;border-radius:8px;padding:0}.swatch.selected{outline:3px solid #222;outline-offset:2px}"
"#colorPanel{display:none;border:1px solid #bbb;border-radius:10px;padding:8px;margin-top:6px}#rgbControls{display:none}#rgbControls label{display:block}#rgbControls input{width:70%;vertical-align:middle}"
".deadzone{margin:16px 0}.deadzone>label{display:block;margin-bottom:3px}.deadzone-control{display:flex;align-items:center;gap:8px}.deadzone-control input{flex:1;min-width:0;margin-left:0}.deadzone-control output{min-width:3em;text-align:right}</style>"
"<h1>RemoteJoy Config</h1><label>Language <select id=langSelect onchange='setLanguage(this.value)'><option value=ja>日本語</option><option value=en>English</option><option value=zh-CN>简体中文</option><option value=zh-TW>繁體中文</option><option value=ko>한국어</option><option value=es>Español</option><option value=fr>Français</option><option value=de>Deutsch</option></select></label><p id=intro></p>"
"<section><h2 id=mapTitle></h2><select id=profile onchange='selectProfile()'></select>"
"<button id=addBtn onclick='addProfile()'></button><button id=deleteBtn onclick='deleteProfile()'></button>"
"<div><span id=ledLabel></span> <button id=colorButton onclick='toggleColorPanel()'></button>"
"<div id=colorPanel>"
"<button class=swatch data-rgb=255,0,0 style='background:#f00' onclick='setPreset(255,0,0)'></button>"
"<button class=swatch data-rgb=0,255,0 style='background:#0f0' onclick='setPreset(0,255,0)'></button>"
"<button class=swatch data-rgb=0,0,255 style='background:#00f' onclick='setPreset(0,0,255)'></button>"
"<button class=swatch data-rgb=255,255,0 style='background:#ff0' onclick='setPreset(255,255,0)'></button>"
"<button class=swatch data-rgb=0,255,255 style='background:#0ff' onclick='setPreset(0,255,255)'></button>"
"<button class=swatch data-rgb=255,0,255 style='background:#f0f' onclick='setPreset(255,0,255)'></button>"
"<button class=swatch data-rgb=255,128,0 style='background:#ff8000' onclick='setPreset(255,128,0)'></button>"
"<button class=swatch data-rgb=255,255,255 style='background:#fff' onclick='setPreset(255,255,255)'></button> "
"<button id=customBtn onclick='showCustomColor()'></button>"
"<div id=rgbControls><label>R <input id=colorR type=range min=0 max=255 oninput='previewColor()' onchange='setCustomColor()'><output id=colorROut></output></label>"
"<label>G <input id=colorG type=range min=0 max=255 oninput='previewColor()' onchange='setCustomColor()'><output id=colorGOut></output></label>"
"<label>B <input id=colorB type=range min=0 max=255 oninput='previewColor()' onchange='setCustomColor()'><output id=colorBOut></output></label></div></div></div>"
"<div class=deadzone><label id=leftLabel for=leftDz></label><div class=deadzone-control><input id=leftDz type=range min=0 max=90 oninput='previewDeadzone()' onchange='setDeadzone()'><output id=leftDzOut for=leftDz></output></div></div>"
"<div class=deadzone><label id=rightLabel for=rightDz></label><div class=deadzone-control><input id=rightDz type=range min=0 max=90 oninput='previewDeadzone()' onchange='setDeadzone()'><output id=rightDzOut for=rightDz></output></div></div>"
"<p id=note></p><div id=map></div>"
"<button id=saveBtn onclick='save()'></button><button id=exportBtn onclick='exportConfig()'></button>"
"<button id=importBtn onclick=importFile.click()></button><input id=importFile type=file accept=application/json hidden onchange='importConfig(this.files[0])'></section>"
"<button id=doneBtn onclick='done()'></button><script>let cfg,L,inputs,targets;"
"const texts={"
"ja:{intro:'ボタンマッピングを編集して「設定を保存」を押してください。「設定完了」で保存して通常モードに戻ります。',mapping:'ボタンマッピング',add:'追加',del:'削除',led:'LED色',custom:'カスタム',leftDz:'左スティック デッドゾーン',rightDz:'右スティック デッドゾーン',note:'START+R3（またはSTART+COMBOを割り当てたボタン）で次のプロファイルに切り替わります。VOL-/VOL+はSTARTと同時に押すと音量調整として機能します（単体ではPSアーカイブスのアナログ左右）。START+→（HOME）、START+←（画面の明るさ）、START+R3は固定です。',save:'設定を保存',exp:'エクスポート',imp:'インポート',done:'設定完了',max:'最大8個です',min:'最低1個必要です',saved:'保存しました',bad:'ファイル形式が正しくありません',imported:'インポートしました',doneTitle:'設定を保存しました',doneMsg:'USBケーブルをPCから外し、PSPに接続してください。'},"
"en:{intro:'Edit the button mapping and press Save settings. Finish setup saves and returns to normal mode.',mapping:'Button mapping',add:'Add',del:'Delete',led:'LED color',custom:'Custom',leftDz:'Left stick dead zone',rightDz:'Right stick dead zone',note:'START+R3 (or START + a button mapped to COMBO) switches to the next profile. VOL-/VOL+ adjust the volume when pressed together with START (alone they are the analog left/right for PS Archives). START+Right (HOME), START+Left (screen brightness) and START+R3 are fixed.',save:'Save settings',exp:'Export',imp:'Import',done:'Finish setup',max:'Maximum of 8 profiles',min:'At least one profile is required',saved:'Saved',bad:'Invalid file format',imported:'Imported',doneTitle:'Settings saved',doneMsg:'Disconnect the USB cable from the PC and connect it to the PSP.'},"
"'zh-CN':{intro:'编辑按键映射后请按“保存设置”。“完成设置”会保存并返回普通模式。',mapping:'按键映射',add:'添加',del:'删除',led:'LED颜色',custom:'自定义',leftDz:'左摇杆死区',rightDz:'右摇杆死区',note:'START+R3（或START+分配了COMBO的按键）可切换到下一个配置。VOL-/VOL+与START同时按下时用于调节音量（单独按下时为PS Archives的模拟左/右）。START+→（HOME）、START+←（屏幕亮度）和START+R3为固定功能。',save:'保存设置',exp:'导出',imp:'导入',done:'完成设置',max:'最多8个配置',min:'至少需要一个配置',saved:'已保存',bad:'文件格式无效',imported:'已导入',doneTitle:'设置已保存',doneMsg:'请从电脑上拔下USB线并连接到PSP。'},"
"'zh-TW':{intro:'編輯按鍵映射後請按「儲存設定」。「完成設定」會儲存並返回一般模式。',mapping:'按鍵映射',add:'新增',del:'刪除',led:'LED顏色',custom:'自訂',leftDz:'左搖桿死區',rightDz:'右搖桿死區',note:'START+R3（或START+指派了COMBO的按鍵）可切換到下一個設定檔。VOL-/VOL+與START同時按下時用於調整音量（單獨按下時為PS Archives的類比左/右）。START+→（HOME）、START+←（螢幕亮度）和START+R3為固定功能。',save:'儲存設定',exp:'匯出',imp:'匯入',done:'完成設定',max:'最多8個設定檔',min:'至少需要一個設定檔',saved:'已儲存',bad:'檔案格式無效',imported:'已匯入',doneTitle:'設定已儲存',doneMsg:'請從電腦上拔下USB線並連接到PSP。'},"
"ko:{intro:'버튼 매핑을 편집한 후 설정 저장을 누르세요. 설정 완료를 누르면 저장하고 일반 모드로 돌아갑니다.',mapping:'버튼 매핑',add:'추가',del:'삭제',led:'LED 색상',custom:'사용자 지정',leftDz:'왼쪽 스틱 데드존',rightDz:'오른쪽 스틱 데드존',note:'START+R3(또는 START+COMBO를 할당한 버튼)로 다음 프로필로 전환됩니다. VOL-/VOL+는 START와 함께 누르면 음량 조절로 작동합니다(단독으로는 PS Archives의 아날로그 좌/우). START+→(HOME), START+←(화면 밝기), START+R3는 고정입니다.',save:'설정 저장',exp:'내보내기',imp:'가져오기',done:'설정 완료',max:'최대 8개입니다',min:'최소 1개가 필요합니다',saved:'저장했습니다',bad:'파일 형식이 올바르지 않습니다',imported:'가져왔습니다',doneTitle:'설정을 저장했습니다',doneMsg:'USB 케이블을 PC에서 분리하고 PSP에 연결하세요.'},"
"es:{intro:'Edite la asignación de botones y pulse Guardar ajustes. Finalizar guarda y vuelve al modo normal.',mapping:'Asignación de botones',add:'Añadir',del:'Eliminar',led:'Color LED',custom:'Personalizado',leftDz:'Zona muerta del stick izquierdo',rightDz:'Zona muerta del stick derecho',note:'START+R3 (o START + un botón asignado a COMBO) pasa al siguiente perfil. VOL-/VOL+ ajustan el volumen al pulsarlos junto con START (solos son el analógico izquierda/derecha de PS Archives). START+Derecha (HOME), START+Izquierda (brillo de pantalla) y START+R3 son fijos.',save:'Guardar ajustes',exp:'Exportar',imp:'Importar',done:'Finalizar',max:'Máximo 8 perfiles',min:'Se requiere al menos un perfil',saved:'Guardado',bad:'Formato de archivo no válido',imported:'Importado',doneTitle:'Ajustes guardados',doneMsg:'Desconecte el cable USB del PC y conéctelo a la PSP.'},"
"fr:{intro:'Modifiez le mappage des boutons puis appuyez sur Enregistrer. Terminer enregistre et revient au mode normal.',mapping:'Mappage des boutons',add:'Ajouter',del:'Supprimer',led:'Couleur LED',custom:'Personnalisée',leftDz:'Zone morte stick gauche',rightDz:'Zone morte stick droit',note:'START+R3 (ou START + un bouton affecté à COMBO) passe au profil suivant. VOL-/VOL+ règlent le volume lorsqu’ils sont pressés avec START (seuls, ils servent d’analogique gauche/droite pour PS Archives). START+Droite (HOME), START+Gauche (luminosité) et START+R3 sont fixes.',save:'Enregistrer',exp:'Exporter',imp:'Importer',done:'Terminer',max:'Maximum 8 profils',min:'Au moins un profil est requis',saved:'Enregistré',bad:'Format de fichier incorrect',imported:'Importé',doneTitle:'Paramètres enregistrés',doneMsg:'Débranchez le câble USB du PC et branchez-le sur la PSP.'},"
"de:{intro:'Tastenbelegung bearbeiten und Einstellungen speichern drücken. Einrichtung beenden speichert und kehrt in den Normalmodus zurück.',mapping:'Tastenbelegung',add:'Hinzufügen',del:'Löschen',led:'LED-Farbe',custom:'Benutzerdefiniert',leftDz:'Totzone linker Stick',rightDz:'Totzone rechter Stick',note:'START+R3 (oder START + eine mit COMBO belegte Taste) wechselt zum nächsten Profil. VOL-/VOL+ regeln die Lautstärke, wenn sie zusammen mit START gedrückt werden (allein sind sie Analog links/rechts für PS Archives). START+Rechts (HOME), START+Links (Bildschirmhelligkeit) und START+R3 sind fest belegt.',save:'Einstellungen speichern',exp:'Exportieren',imp:'Importieren',done:'Einrichtung beenden',max:'Maximal 8 Profile',min:'Mindestens ein Profil ist erforderlich',saved:'Gespeichert',bad:'Ungültiges Dateiformat',imported:'Importiert',doneTitle:'Einstellungen gespeichert',doneMsg:'USB-Kabel vom PC trennen und mit der PSP verbinden.'}};"
"function setLanguage(code){L=texts[code]||texts.en;langSelect.value=texts[code]?code:'en';localStorage.setItem('rjmLang',langSelect.value);document.documentElement.lang=langSelect.value;"
"intro.textContent=L.intro;mapTitle.textContent=L.mapping;addBtn.textContent=L.add;deleteBtn.textContent=L.del;ledLabel.textContent=L.led;customBtn.textContent=L.custom;leftLabel.textContent=L.leftDz;rightLabel.textContent=L.rightDz;note.textContent=L.note;saveBtn.textContent=L.save;exportBtn.textContent=L.exp;importBtn.textContent=L.imp;doneBtn.textContent=L.done;"
/* Same 22 rows as the Pico 2 W build so exported JSON stays compatible.
 * System / Misc (16, 17) do not exist on a PS2 controller and are hidden. */
"inputs=['Cross','Circle','Square','Triangle','D-pad Up','D-pad Right','D-pad Down','D-pad Left','L1','R1','L2','R2','L3','R3','Select','Start','System','Misc','Right stick Up','Right stick Right','Right stick Down','Right stick Left'];"
"targets=['None','SELECT','START','D-PAD UP','D-PAD RIGHT','D-PAD DOWN','D-PAD LEFT','L','R','TRIANGLE','CIRCLE','CROSS','SQUARE','HOME','VOL+','VOL-','NOTE','COMBO'];if(cfg)render()}"
"function initialLanguage(){let s=localStorage.getItem('rjmLang');if(s&&texts[s])return s;let n=(navigator.language||'en').toLowerCase();if(n.startsWith('zh'))return n.includes('tw')||n.includes('hk')?'zh-TW':'zh-CN';for(let x of ['ja','ko','es','fr','de'])if(n.startsWith(x))return x;return'en'}"
"async function post(path){let r=await fetch(path,{method:'POST'});if(!r.ok)throw Error(await r.text()||r.status)}"
"async function loadConfig(){cfg=await(await fetch('/api/config')).json();render()}"
"function render(){profile.innerHTML=cfg.profiles.map((x,i)=>'<option value='+i+(i==cfg.active?' selected':'')+'>'+x.name+'</option>').join('');"
"let x=cfg.profiles[cfg.active];let hex='#'+x.color.map(v=>v.toString(16).padStart(2,'0')).join('');colorButton.style.background=hex;[colorR.value,colorG.value,colorB.value]=x.color;[colorROut.value,colorGOut.value,colorBOut.value]=x.color;document.querySelectorAll('.swatch').forEach(e=>e.classList.toggle('selected',e.dataset.rgb==x.color.join(',')));leftDz.value=x.deadzone[0];rightDz.value=x.deadzone[1];leftDzOut.value=x.deadzone[0]+'%';rightDzOut.value=x.deadzone[1]+'%';"
"map.innerHTML='<table>'+inputs.map((n,i)=>i==16||i==17?'':'<tr><td>'+n+'</td><td><select onchange=bind('+i+',this.value)>'+targets.map((t,j)=>'<option value='+j+(x.outputs[i]==j?' selected':'')+'>'+t+'</option>').join('')+'</select></td></tr>').join('')+'</table>'}"
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
"async function save(){await post('/api/config/save');alert(L.saved)}"
"function exportConfig(){let a=document.createElement('a');a.href=URL.createObjectURL(new Blob([JSON.stringify(cfg,null,2)],{type:'application/json'}));a.download='remotejoy-mapping.json';a.click();URL.revokeObjectURL(a.href)}"
"async function importConfig(f){try{let c=JSON.parse(await f.text());if(c.version!=2||!Array.isArray(c.profiles)||!c.profiles.length||c.profiles.length>8)throw 0;"
"for(let x of c.profiles)if(!Array.isArray(x.color)||x.color.length!=3||!Array.isArray(x.deadzone)||x.deadzone.length!=2||!Array.isArray(x.outputs)||x.outputs.length!=inputs.length||x.color.some(v=>v<0||v>255)||x.deadzone.some(v=>v<0||v>90)||x.outputs.some(v=>v<0||v>=targets.length))throw 0;"
"await post('/api/profile/reset');for(let i=1;i<c.profiles.length;i++)await post('/api/profile/add');"
"for(let i=0;i<c.profiles.length;i++){let x=c.profiles[i];await post('/api/profile/color/'+i+'/'+x.color.join('/'));await post('/api/profile/deadzone/'+i+'/'+x.deadzone.join('/'));for(let t=0;t<inputs.length;t++)await post('/api/profile/bind/'+i+'/'+t+'/'+x.outputs[t]);"
"if(typeof x.name=='string'&&x.name)try{await post('/api/profile/name/'+i+'/'+encodeURIComponent(x.name))}catch(e){}}"
"await post('/api/profile/select/'+Math.min(c.active||0,c.profiles.length-1));await post('/api/config/save');await loadConfig();alert(L.imported)}catch(e){alert(L.bad)}}"
"async function done(){await post('/api/done');document.body.innerHTML='<h1>'+L.doneTitle+'</h1><p>'+L.doneMsg+'</p>'}"
"setLanguage(initialLanguage());loadConfig()</script></html>";

/* ---- HTTP ---- */

static void send_response(struct tcp_pcb *pcb, const char *status, const char *type,
                          const char *body, size_t length)
{
    char header[160];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                     status, type, (unsigned)length);
    tcp_write(pcb, header, (u16_t)n, TCP_WRITE_FLAG_COPY);
    if (length) tcp_write(pcb, body, (u16_t)length, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
}

static void send_status(struct tcp_pcb *pcb, bool ok, const char *error_status)
{
    send_response(pcb, ok ? "204 No Content" : error_status, "text/plain", "", 0);
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
        if (chunk > TCP_MSS) chunk = TCP_MSS;
        if (chunk > available) chunk = available;
        /* The page is const and stays in flash, so it needs no copy. */
        if (chunk && tcp_write(pcb, stream->body + stream->offset, chunk, 0) == ERR_OK) {
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
                     "Content-Length: %u\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                     (unsigned)length);
    stream->body = body;
    stream->length = length;
    tcp_arg(pcb, stream);
    tcp_sent(pcb, stream_sent);
    tcp_err(pcb, stream_error);
    if (tcp_write(pcb, header, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        tcp_arg(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_err(pcb, NULL);
        free(stream);
        return false;
    }
    stream_sent(stream, pcb, 0);
    return true;
}

static void make_config(char *out, size_t size)
{
    size_t used = 0;
#define APPEND(...) do { if (used < size) { int n = snprintf(out + used, size - used, __VA_ARGS__); if (n > 0) used += (size_t)n; } } while (0)
    APPEND("{\"version\":%u,\"active\":%u,\"profiles\":[",
           (unsigned)g_rjm_mapping.version, (unsigned)g_rjm_mapping.active_profile);
    for (uint8_t p = 0; p < g_rjm_mapping.profile_count; ++p) {
        const struct RjmMappingProfile *profile = &g_rjm_mapping.profile[p];
        APPEND("%s{\"name\":\"%.23s\",\"color\":[%u,%u,%u],\"deadzone\":[%u,%u],\"outputs\":[",
               p ? "," : "", profile->name, profile->color.red, profile->color.green,
               profile->color.blue, profile->left_deadzone_percent,
               profile->right_deadzone_percent);
        for (uint8_t b = 0; b < RJM_MAPPABLE_INPUT_COUNT; ++b)
            APPEND("%s%u", b ? "," : "", profile->output[b]);
        APPEND("]}");
    }
    APPEND("]}");
#undef APPEND
}

/* Import starts from a single default profile and adds the rest. */
static void reset_mapping(void)
{
    rjm_mapping_set_defaults(&g_rjm_mapping);
    g_rjm_mapping.profile_count = 1;
}

static bool add_profile(void)
{
    if (g_rjm_mapping.profile_count >= RJM_PROFILE_MAX) return false;
    uint8_t index = g_rjm_mapping.profile_count++;
    g_rjm_mapping.profile[index] = g_rjm_mapping.profile[g_rjm_mapping.active_profile];
    snprintf(g_rjm_mapping.profile[index].name, sizeof(g_rjm_mapping.profile[index].name),
             "Profile %u", (unsigned)(index + 1));
    g_rjm_mapping.active_profile = index;
    return true;
}

static bool delete_profile(unsigned index)
{
    if (g_rjm_mapping.profile_count <= 1 || index >= g_rjm_mapping.profile_count) return false;
    memmove(&g_rjm_mapping.profile[index], &g_rjm_mapping.profile[index + 1],
            (g_rjm_mapping.profile_count - index - 1) * sizeof(g_rjm_mapping.profile[0]));
    --g_rjm_mapping.profile_count;
    if (g_rjm_mapping.active_profile >= g_rjm_mapping.profile_count)
        g_rjm_mapping.active_profile = g_rjm_mapping.profile_count - 1;
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

/* Names are echoed into JSON and the page's HTML, so reject characters that
 * would need escaping there. */
static bool set_profile_name(unsigned index, const char *encoded, size_t length)
{
    char name[RJM_PROFILE_NAME_MAX];
    if (index >= g_rjm_mapping.profile_count ||
        !decode_url_component(name, sizeof(name), encoded, length) || !name[0])
        return false;
    for (const char *c = name; *c; ++c)
        if ((uint8_t)*c < 0x20 || *c == '"' || *c == '\\' || *c == '<' || *c == '>' || *c == '&')
            return false;
    memcpy(g_rjm_mapping.profile[index].name, name, sizeof(name));
    return true;
}

static err_t http_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    char request[384] = {0};
    unsigned a, b, c, d;
    if (!p) {
        if (arg) {
            free(arg);
            tcp_arg(pcb, NULL);
            tcp_sent(pcb, NULL);
            tcp_err(pcb, NULL);
        }
        tcp_close(pcb);
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }
    pbuf_copy_partial(p, request,
                      p->tot_len < sizeof(request) - 1 ? p->tot_len : sizeof(request) - 1, 0);
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    if (strncmp(request, "POST /api/done ", 15) == 0) {
        rjm_mapping_save();
        send_response(pcb, "202 Accepted", "text/plain", "", 0);
        g_reboot_pending = true;
        g_reboot_at_ms = to_ms_since_boot(get_absolute_time()) + REBOOT_DELAY_MS;
    } else if (strncmp(request, "GET /api/config ", 16) == 0) {
        make_config(g_json, sizeof(g_json));
        send_response(pcb, "200 OK", "application/json", g_json, strlen(g_json));
    } else if (strncmp(request, "POST /api/config/save ", 22) == 0) {
        send_status(pcb, rjm_mapping_save(), "500 Internal Server Error");
    } else if (strncmp(request, "POST /api/profile/reset ", 24) == 0) {
        reset_mapping();
        send_status(pcb, true, "");
    } else if (strncmp(request, "POST /api/profile/add ", 22) == 0) {
        send_status(pcb, add_profile(), "409 Conflict");
    } else if (sscanf(request, "POST /api/profile/delete/%u ", &a) == 1) {
        send_status(pcb, delete_profile(a), "409 Conflict");
    } else if (sscanf(request, "POST /api/profile/select/%u ", &a) == 1) {
        bool ok = a < g_rjm_mapping.profile_count;
        if (ok) g_rjm_mapping.active_profile = (uint8_t)a;
        send_status(pcb, ok, "400 Bad Request");
    } else if (sscanf(request, "POST /api/profile/color/%u/%u/%u/%u ", &a, &b, &c, &d) == 4) {
        bool ok = a < g_rjm_mapping.profile_count && b <= 255 && c <= 255 && d <= 255;
        if (ok) g_rjm_mapping.profile[a].color = (struct RjmRgb){(uint8_t)b, (uint8_t)c, (uint8_t)d};
        send_status(pcb, ok, "400 Bad Request");
    } else if (sscanf(request, "POST /api/profile/deadzone/%u/%u/%u ", &a, &b, &c) == 3) {
        bool ok = a < g_rjm_mapping.profile_count && b <= 90 && c <= 90;
        if (ok) {
            g_rjm_mapping.profile[a].left_deadzone_percent = (uint8_t)b;
            g_rjm_mapping.profile[a].right_deadzone_percent = (uint8_t)c;
        }
        send_status(pcb, ok, "400 Bad Request");
    } else if (sscanf(request, "POST /api/profile/bind/%u/%u/%u ", &a, &b, &c) == 3) {
        bool ok = a < g_rjm_mapping.profile_count && b < RJM_MAPPABLE_INPUT_COUNT &&
                  c < RJM_OUTPUT_COUNT;
        if (ok) g_rjm_mapping.profile[a].output[b] = (uint8_t)c;
        send_status(pcb, ok, "400 Bad Request");
    } else if (sscanf(request, "POST /api/profile/name/%u/", &a) == 1) {
        char *start = strchr(request + 23, '/');
        char *space = start ? strchr(start + 1, ' ') : NULL;
        send_status(pcb, space && set_profile_name(a, start + 1, (size_t)(space - start - 1)),
                    "400 Bad Request");
    } else if (strncmp(request, "GET ", 4) == 0) {
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

static bool http_start(void)
{
    struct tcp_pcb *server = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!server || tcp_bind(server, IP_ANY_TYPE, HTTP_PORT) != ERR_OK) return false;
    server = tcp_listen_with_backlog(server, 4);
    if (!server) return false;
    tcp_accept(server, http_accept);
    return true;
}

/* ---- TinyUSB NCM <-> lwIP glue (from the TinyUSB net_lwip_webserver example) ---- */

static err_t linkoutput_fn(struct netif *netif, struct pbuf *p)
{
    (void)netif;
    for (;;) {
        if (!tud_ready()) return ERR_USE;
        if (tud_network_can_xmit(p->tot_len)) {
            tud_network_xmit(p, 0);
            return ERR_OK;
        }
        tud_task();
    }
}

static err_t ip4_output_fn(struct netif *netif, struct pbuf *p, const ip4_addr_t *addr)
{
    return etharp_output(netif, p, addr);
}

static err_t netif_init_cb(struct netif *netif)
{
    netif->mtu = CFG_TUD_NET_MTU;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
    netif->state = NULL;
    netif->name[0] = 'E';
    netif->name[1] = 'X';
    netif->linkoutput = linkoutput_fn;
    netif->output = ip4_output_fn;
    return ERR_OK;
}

bool tud_network_recv_cb(const uint8_t *src, uint16_t size)
{
    if (g_received_frame) return false;
    if (size) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
        if (p) {
            pbuf_take(p, src, size);
            g_received_frame = p;
        }
    }
    return true;
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg)
{
    struct pbuf *p = (struct pbuf *)ref;
    (void)arg;
    return pbuf_copy_partial(p, dst, p->tot_len, 0);
}

void tud_network_init_cb(void)
{
    if (g_received_frame) {
        pbuf_free(g_received_frame);
        g_received_frame = NULL;
    }
}

static void service_traffic(void)
{
    if (g_received_frame) {
        if (ethernet_input(g_received_frame, &g_netif) != ERR_OK) pbuf_free(g_received_frame);
        g_received_frame = NULL;
        tud_network_recv_renew();
    }
    sys_check_timeouts();
}

/* lwIP NO_SYS hooks */
sys_prot_t sys_arch_protect(void) { return 0; }
void sys_arch_unprotect(sys_prot_t pval) { (void)pval; }
uint32_t sys_now(void) { return to_ms_since_boot(get_absolute_time()); }

static void update_led(rjm_portal_led_fn set_led)
{
    /* Slow red breathing, as on the Pico 2 W. */
    uint32_t phase = to_ms_since_boot(get_absolute_time()) % LED_BREATH_PERIOD_MS;
    uint32_t half = LED_BREATH_PERIOD_MS / 2;
    uint32_t level = phase < half ? phase : LED_BREATH_PERIOD_MS - phase;
    level = level * 255u / half;
    set_led((uint8_t)(level * level / 255u), 0, 0);
}

void rjm_config_portal_run(rjm_portal_led_fn set_led)
{
    const tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_FULL,
    };

    printf("Web configuration mode: connect USB to a PC and open http://192.168.7.1/\n");
    tusb_init(0, &dev_init);

    lwip_init();
    g_netif.hwaddr_len = sizeof(tud_network_mac_address);
    memcpy(g_netif.hwaddr, tud_network_mac_address, sizeof(tud_network_mac_address));
    g_netif.hwaddr[5] ^= 0x01;
    netif_add(&g_netif, &k_ipaddr, &k_netmask, &k_gateway, NULL, netif_init_cb, ip_input);
    netif_set_default(&g_netif);

    while (dhserv_init(&k_dhcp_config) != ERR_OK) tud_task();
    if (!http_start()) printf("HTTP server start failed\n");

    for (;;) {
        tud_task();
        service_traffic();
        update_led(set_led);
        if (g_reboot_pending &&
            (int32_t)(to_ms_since_boot(get_absolute_time()) - g_reboot_at_ms) >= 0) {
            printf("Web configuration finished; rebooting into normal mode\n");
            set_led(0, 0, 0);
            watchdog_reboot(0, 0, 0);
            for (;;) tight_loop_contents();
        }
    }
}
