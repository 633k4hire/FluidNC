"use strict";

const $=selector=>document.querySelector(selector);
const $$=selector=>[...document.querySelectorAll(selector)];
const state={
  csrf:"",control:"",locked:true,fresh:false,lathe:true,
  telemetry:null,settings:[],settingsTab:"Flash/Settings",
  jogIncrement:1,firmware:null,package:{controller:null,dial:null},
  currentFile:""
};
const esc=value=>String(value??"—").replace(/[&<>"']/g,char=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[char]));

function showToast(message,type="info",timeout=4500){
  const toast=document.createElement("div");
  toast.className=`toast ${type}`;toast.textContent=message;
  $("#toast-region").append(toast);
  setTimeout(()=>toast.remove(),timeout);
}

async function textFetch(url,options={}){
  const response=await fetch(url,{cache:"no-store",credentials:"same-origin",...options});
  const text=await response.text();
  if(!response.ok){
    let message=text||`${response.status} ${response.statusText}`;
    try{message=JSON.parse(text).error||message;}catch(_){}
    throw new Error(message);
  }
  return text;
}
function extractJson(text){
  const first=text.indexOf("{"),array=text.indexOf("[");
  let start=first<0?array:array<0?first:Math.min(first,array);
  if(start<0)throw new Error("Controller returned no JSON");
  const open=text[start],end=text.lastIndexOf(open==="{"?"}":"]");
  if(end<start)throw new Error("Controller returned incomplete JSON");
  return JSON.parse(text.slice(start,end+1));
}
async function jsonFetch(url,options={}){return extractJson(await textFetch(url,options));}
async function jsonFetchTimeout(url,options={},timeoutMs=2500){
  const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),timeoutMs);
  try{return await jsonFetch(url,{...options,signal:controller.signal});}
  finally{clearTimeout(timer);}
}
function writeHeaders(extra={}){
  return {"X-CSRF-Token":state.csrf,"X-TAMS-Control-Token":state.control,...extra};
}
async function renewConsoleSession(){
  const session=await jsonFetch("/api/v1/console/session");
  state.csrf=session.csrf_token||"";
  if(!state.csrf)throw new Error("Controller returned no console CSRF token");
  return session;
}

function setPage(name){
  $$(".page").forEach(element=>element.classList.toggle("active",element.id===`page-${name}`));
  $$("nav button").forEach(element=>element.classList.toggle("active",element.dataset.page===name));
  if(name==="settings"&&!state.settings.length)refreshSettings();
  if(name==="files")refreshFiles();
  if(name==="firmware")refreshFirmware();
}
$$("nav button").forEach(button=>button.onclick=()=>setPage(button.dataset.page));

function applyTheme(theme){
  document.documentElement.dataset.theme=theme;
  localStorage.setItem("xza-theme",theme);
  const dark=theme==="dark";
  $("#theme-icon").textContent=dark?"☀️":"🌙";
  $("#theme-toggle").setAttribute("aria-label",dark?"Switch to light theme":"Switch to dark theme");
  document.querySelector('meta[name="theme-color"]').content=dark?"#0c1117":"#eef3f7";
}
const savedTheme=localStorage.getItem("xza-theme");
applyTheme(savedTheme||(matchMedia("(prefers-color-scheme: dark)").matches?"dark":"light"));
$("#theme-toggle").onclick=()=>applyTheme(document.documentElement.dataset.theme==="dark"?"light":"dark");

function updateLockUi(){
  $("#lock-label").textContent=state.locked?"Locked":"Unlocked";
  $("#lock-icon").textContent=state.locked?"🔒":"🔓";
  $("#lock-toggle").className=`lock-button ${state.locked?"locked":"unlocked"}`;
  $("#lock-toggle").setAttribute("aria-pressed",String(!state.locked));
  $("#lock-toggle").title=state.locked?"Unlock controls":"Lock controls";
  $("#jog-lock-state").textContent=state.locked?"Locked":"Unlocked";
  $("#jog-lock-state").className=`pill ${state.locked?"":"good"}`;
  $("#diag-lock").textContent=state.locked?"Locked":"Unlocked";
  $$("[data-write]").forEach(element=>element.disabled=state.locked||element.dataset.serverDisabled==="true");
  $("#control-disabled-reason").textContent=state.locked?"Unlock this browser tab to operate the lathe.":"Direct operator control is active. Controller-native limits and alarms remain authoritative.";
  updateFirmwareButtons();
  updateCControls();
}

async function initializeConsole(){
  try{
    await renewConsoleSession();
    state.control="";state.locked=true;updateLockUi();
  }catch(error){
    $("#lock-toggle").disabled=true;
    showToast(`Console session unavailable: ${error.message}`,"error",8000);
  }
}
$("#lock-toggle").onclick=async()=>{
  try{
    if(state.locked){
      // The controller keeps console sessions in RAM. Renew here so a browser
      // tab left open across a controller restart can unlock without reloading.
      await renewConsoleSession();
      const result=await jsonFetch("/api/v1/console/unlock",{method:"POST",headers:{"X-CSRF-Token":state.csrf}});
      state.control=result.control_token;state.locked=false;showToast("Controls unlocked","success");
    }else{
      await jsonFetch("/api/v1/console/lock",{method:"POST",headers:writeHeaders()});
      state.control="";state.locked=true;showToast("Controls locked");
    }
    updateLockUi();
  }catch(error){showToast(error.message,"error");}
};

function number(value,fallback="—"){
  if(value===null||value===undefined)return fallback;
  const parsed=Number(value);return Number.isFinite(parsed)?parsed.toFixed(3):fallback;
}
function integer(value,fallback="—"){
  const parsed=Number(value);return Number.isFinite(parsed)?Math.trunc(parsed).toLocaleString():fallback;
}
function renderStations(container,active,target,controls=false){
  container.innerHTML="";
  for(let tool=1;tool<=5;tool++){
    const element=document.createElement(controls?"button":"span");
    element.className=`station${Number(active)===tool?" active":""}${Number(target)===tool?" target":""}`;
    element.textContent=`T${tool}`;
    if(controls){
      element.dataset.control="turret";element.dataset.tool=String(tool);
      element.dataset.write="";element.disabled=state.locked;
    }
    container.append(element);
  }
}

function renderEncoderDiagnostics(spindle,encoder){
  const pulses=Number(encoder.pulse_count)||0,indexes=Number(encoder.index_count)||0;
  const direction=encoder.has_direction?String(encoder.direction).toUpperCase():"UNKNOWN";
  const fraction=Number(encoder.angular_position_revolution);
  const angle=Number.isFinite(fraction)?((fraction%1)+1)%1*360:0;
  const live=!!encoder.capture_active&&!encoder.stale;
  $("#encdiag-live").textContent=live?"Live":encoder.capture_active?"Stopped / stale":"Inactive";
  $("#encdiag-live").className=`pill ${live?"good":encoder.capture_active?"":"danger"}`;
  $("#encdiag-capture").textContent=encoder.capture_active?"Capture active":"Capture inactive";
  $("#encdiag-capture").className=`pill ${encoder.capture_active?"good":"danger"}`;
  $("#encdiag-a").classList.toggle("good",pulses>0);$("#encdiag-a-status").textContent=pulses>0?`${integer(pulses)} edges`:"No pulse captured";
  $("#encdiag-b").classList.toggle("good",!!encoder.has_direction);$("#encdiag-b-status").textContent=encoder.has_direction?`${direction} decoded`:"No direction decoded";
  $("#encdiag-i").classList.toggle("good",indexes>0);$("#encdiag-i-status").textContent=indexes>0?`${integer(indexes)} captured`:"Not seen (optional)";
  $("#encdiag-needle").style.transform=`rotate(${angle.toFixed(3)}deg)`;$("#encdiag-degrees").textContent=`${angle.toFixed(1)}°`;
  $("#encdiag-cw").classList.toggle("active",direction==="CW");$("#encdiag-ccw").classList.toggle("active",direction==="CCW");
  $("#encdiag-rpm").textContent=encoder.has_measured_rpm?`${number(spindle.measured_rpm)} RPM`:"—";
  $("#encdiag-direction").textContent=direction;$("#encdiag-ppr").textContent=integer(encoder.pulses_per_revolution);
  $("#encdiag-pulses").textContent=integer(pulses);$("#encdiag-indexes").textContent=integer(indexes);
  $("#encdiag-index-interval").textContent=Number(encoder.last_index_pulses)>0?`${integer(encoder.last_index_pulses)} pulses`:"Not measured";
  $("#encdiag-age").textContent=pulses>0?`${integer(encoder.last_pulse_age_ms)} ms`:"No pulse captured";
}

function renderTelemetry(data){
  state.telemetry=data;state.fresh=true;
  const machine=data.machine||{},execution=data.execution||{},positions=data.positions||{};
  const spindle=data.spindle||{},encoder=spindle.encoder||{},turret=data.turret||{};
  state.lathe=/lathe/i.test(machine.model||"")||spindle.c_axis==="C";
  $("#mode-banner").hidden=state.lathe;
  if(!state.lathe){
    $("#mode-banner").textContent="The controller did not report the Maijker lathe telemetry contract.";
    $$("[data-lathe]").forEach(element=>element.hidden=true);
  }
  const x=positions.x||{},z=positions.z||{},c=positions.c||{};
  const wcs=execution.coordinate_system||"—";
  $("#dro-x").textContent=number(x.work);$("#dro-x-work").textContent=number(x.machine);
  $("#dro-z").textContent=number(z.work);$("#dro-z-work").textContent=number(z.machine);
  $("#dro-x-wcs").textContent=`${wcs} work`;$("#dro-z-wcs").textContent=`${wcs} work`;
  $("#dro-c").textContent=number(c.machine);$("#c-angle").textContent=number(c.machine);
  $("#control-x-machine").textContent=number(x.machine);$("#control-x-work").textContent=`${number(x.work)} work`;
  $("#control-z-machine").textContent=number(z.machine);$("#control-z-work").textContent=`${number(z.work)} work`;
  $("#dro-x-mode").textContent=`${spindle.diameter_mode||"unknown"} mode`;
  $("#coordinate-mode").textContent=`${wcs} work / ${execution.distance_mode||"—"}`;
  if(document.activeElement!==$("#coordinate-system")&&wcs!=="—")$("#coordinate-system").value=wcs;
  $("#feed-mode").textContent=execution.feed_mode||"—";
  const activeLimits=[["X",x],["Z",z],["C",c]].filter(([,axis])=>axis.limit_active).map(([name])=>name);
  $("#limits").textContent=activeLimits.length?activeLimits.join(", "):"Clear";
  $("#alarm").textContent=machine.alarm||"None";
  $("#machine-state").textContent=machine.state||"Unknown";
  const homed=[x,z].every(axis=>axis.available&&axis.homed);
  $("#homed").textContent=homed?"X / Z Homed":"Unhomed";
  $("#homed").className=`pill ${homed?"good":"danger"}`;

  const active=turret.current_station,target=turret.target_station;
  renderStations($("#turret-stations"),active,target);
  renderStations($("#turret-control-stations"),active,target,true);
  $("#turret-tools").textContent=`T${active??0} / T${target??0}`;
  $("#turret-confirmed").textContent=turret.software_position_known?turret.mechanically_confirmed?"Mechanically confirmed":"Software position known":"Unconfirmed";
  $("#turret-state").textContent=turret.last_error||"Idle";
  $("#turret-control-status").textContent=turret.target_station?"Moving":turret.last_error||"Idle";
  $("#turret-sensor").textContent=turret.sensor_configured?"Installed":"Not installed";
  $("#turret-warning").hidden=!!turret.sensor_configured;
  const cuttingTools=data.assets?.cutting_tools||[];
  for(const tool of cuttingTools){
    const row=$(`#tool-table tr[data-tool="${Number(tool.station)}"]`);if(!row)continue;
    const values={gx:tool.geometry_x_mm,gz:tool.geometry_z_mm,wx:tool.wear_x_mm,wz:tool.wear_z_mm,nr:tool.nose_radius_mm,o:tool.orientation};
    for(const [field,value] of Object.entries(values)){
      const input=row.querySelector(`[data-field="${field}"]`);
      if(input&&document.activeElement!==input&&value!==null&&value!==undefined)input.value=value;
    }
  }

  const encoderReady=!!encoder.configured;
  $("#encoder-state").textContent=encoderReady?"Configured":"Not commissioned";
  $("#encoder-state").className=`pill ${encoderReady?"good":"danger"}`;
  $("#encoder-rpm").textContent=spindle.measured_rpm===null?"—":number(spindle.measured_rpm);
  $("#encoder-phase").textContent=`${encoder.has_index?"Index seen":"No index"} / ${number(encoder.angular_position_revolution)}`;
  $("#encoder-freshness").textContent=encoder.fault?"Fault":encoder.stale?"Stale":encoderReady?"Current":"Unavailable";
  $("#threading").textContent="Disabled";
  $("#spindle-command").textContent=`${number(spindle.commanded_rpm,"0")} RPM / ${spindle.state||"OFF"}`;
  $("#spindle-measured").textContent=encoderReady?`${number(spindle.measured_rpm)} RPM`:"Not commissioned";
  $("#spindle-mode").textContent=spindle.speed_mode||"FIXED_RPM";
  $("#c-position").textContent=encoder.has_angular_position?number(c.machine):"Unavailable";
  $("#c-capability").textContent=encoderReady?"Encoder configured; controller health remains authoritative":"Angular positioning not commissioned";

  const owner=String(spindle.mode||"IDLE").toUpperCase();
  $("#chuck-owner").textContent=`Owner: ${owner}`;$("#spindle-owner").textContent=owner;
  $$(".mode-switch button").forEach(button=>button.classList.toggle("active",button.dataset.mode.toUpperCase()===owner));
  const cMode=/C.?POSITION/.test(owner);
  $("#spindle-motion-panel").hidden=cMode;$("#c-motion-panel").hidden=!cMode;

  const commanded=Number(spindle.commanded_rpm)||0,measured=spindle.measured_rpm;
  const maximumRpm=Math.max(1,Number(spindle.maximum_rpm)||500);
  $("#spindle-rpm").max=String(maximumRpm);
  $("#spindle-slider").max=String(maximumRpm);
  if(Number($("#spindle-rpm").value)>maximumRpm)$("#spindle-rpm").value=String(maximumRpm);
  $("#gauge-rpm").textContent=Math.round(measured??commanded);
  $("#gauge-direction").textContent=spindle.state||"OFF";
  $("#rpm-gauge").style.setProperty("--rpm-pct",`${Math.min(75,Math.max(0,commanded/maximumRpm*75))}%`);
  $("#control-commanded-rpm").textContent=Math.round(commanded);
  $("#control-measured-rpm").textContent=measured===null?"—":Math.round(measured);
  $("#control-encoder-state").textContent=encoderReady?encoder.fault?"Fault":encoder.stale?"Stale":"Ready":"Not commissioned";
  renderEncoderDiagnostics(spindle,encoder);
  state.cReady=encoderReady&&encoder.has_angular_position;
  updateCControls();
  $("#connection-dot").className="dot online";$("#connection-label").textContent="Controller online";
  $("#diag-telemetry").textContent=`Sequence ${data.sequence??"—"} / current`;
  $("#raw-reports").textContent=JSON.stringify(data,null,2);
}

function updateCControls(){
  const reason=state.cReady?"C-axis feedback is available.":"C positioning requires commissioned angular feedback.";
  $("#c-disabled-reason").textContent=reason;
  $$('[data-control="jog"][data-axis="C"]').forEach(button=>{
    button.dataset.serverDisabled=String(!state.cReady);
    button.disabled=state.locked||!state.cReady;
  });
}

async function refreshStatus(){
  try{
    const telemetry=await jsonFetchTimeout("/api/v1/lathe/status",{},2000);
    renderTelemetry(telemetry);
  }catch(error){
    state.fresh=false;
    $("#connection-dot").className="dot offline";$("#connection-label").textContent="Controller unavailable";
    $("#machine-state").textContent="Stale";$("#diag-telemetry").textContent="Stale";
  }
}

async function statusPoll(){
  await refreshStatus();
  const encoderLive=$("#page-encoder").classList.contains("active")&&document.visibilityState==="visible";
  setTimeout(statusPoll,encoderLive?750:1500);
}

async function typedAction(type,body={}){
  try{
    await textFetch(`/api/v1/lathe/${type}`,{
      method:"POST",headers:writeHeaders({"Content-Type":"application/json"}),body:JSON.stringify(body)
    });
    showToast(`${type.replace("-"," ")} accepted`,"success",2500);
    setTimeout(refreshStatus,200);
  }catch(error){showToast(error.message,"error",7000);}
}

document.addEventListener("click",event=>{
  const button=event.target.closest("[data-control]");if(!button||button.disabled)return;
  const type=button.dataset.control;
  if(type==="jog"){
    const axis=button.dataset.axis;
    typedAction("jog",{axis,direction:Number(button.dataset.direction),increment:axis==="C"?Number($("#c-increment").value):state.jogIncrement,feed:axis==="C"?Number($("#c-feed").value):Number($("#jog-feed").value)});
  }else if(type==="home"){
    typedAction("home",{axis:button.dataset.axis});
  }else if(type==="spindle"){
    typedAction("spindle",{direction:button.dataset.direction,rpm:Number($("#spindle-rpm").value)});
  }else if(type==="spindle-stop"||type==="jog-cancel"){
    typedAction(type);
  }else if(type==="turret"){
    typedAction("turret",{tool:Number(button.dataset.tool)});
  }else if(type==="chuck"){
    typedAction("chuck",{mode:button.dataset.mode});
  }
});

$("#jog-increments").onclick=event=>{
  const button=event.target.closest("[data-increment]");if(!button)return;
  state.jogIncrement=Number(button.dataset.increment);
  $$("#jog-increments button").forEach(item=>item.classList.toggle("active",item===button));
};
$("#spindle-rpm").oninput=event=>{
  const maximum=Number(event.target.max)||500;
  const bounded=Math.min(maximum,Math.max(0,Number(event.target.value)||0));
  event.target.value=String(bounded);$("#spindle-slider").value=String(bounded);
};
$("#spindle-slider").oninput=event=>{$("#spindle-rpm").value=event.target.value;};
$("#set-physical-station").onclick=()=>typedAction("turret/confirm",{tool:Number($("#physical-station").value),visual_inspection:true});
$("#apply-coordinate-system").onclick=()=>typedAction("coordinate-system",{coordinate_system:$("#coordinate-system").value});

function toolRows(){
  $("#tool-table").innerHTML=[1,2,3,4,5].map(tool=>`<tr data-tool="${tool}"><td>T${tool}${tool===5?" / probe":""}</td>${["gx","gz","wx","wz","nr","o"].map(field=>`<td><input data-field="${field}" type="number" step="0.001" value="0"></td>`).join("")}<td><button data-save-tool="${tool}" data-write>Save</button></td></tr>`).join("");
  updateLockUi();
}
$("#tool-table").onclick=event=>{
  const button=event.target.closest("[data-save-tool]");if(!button||button.disabled)return;
  const row=button.closest("tr"),tool=Number(button.dataset.saveTool);
  const value=name=>Number(row.querySelector(`[data-field="${name}"]`).value||0);
  typedAction("tool",{tool,geometry_x:value("gx"),geometry_z:value("gz"),wear_x:value("wx"),wear_z:value("wz"),nose_radius:value("nr"),orientation:value("o")});
};
$("#apply-touch-off").onclick=()=>{
  const axis=$("#touch-axis").value;
  typedAction("touch-off",{tool:Number($("#touch-tool").value),axis,machine:Number($("#touch-machine").value),reference:Number($("#touch-reference").value),mode:axis==="X"?$("#touch-mode").value:"radius"});
};
$("#refresh-tools").onclick=refreshStatus;

function settingInput(item,index){
  const type=String(item.T||item.type||"S"),value=item.V??item.value??"";
  if(item.W===0||item.W===false||type==="P"){
    return `<code class="readonly-setting">${esc(value)}</code>`;
  }
  const options=item.O||item.options;
  if(Array.isArray(options)&&options.length){
    return `<select data-setting-input="${index}">${options.map(option=>{
      const entry=option&&typeof option==="object"?Object.entries(option)[0]:null;
      const optionValue=option?.value??option?.id??option?.V??entry?.[1]??option;
      const optionName=option?.name??option?.label??option?.id??entry?.[0]??optionValue;
      return `<option value="${esc(optionValue)}"${String(optionValue)===String(value)?" selected":""}>${esc(optionName)}</option>`;
    }).join("")}</select>`;
  }
  if(type==="B"){
    return `<select data-setting-input="${index}"><option value="false"${/^(0|false)$/i.test(String(value))?" selected":""}>False</option><option value="true"${/^(1|true)$/i.test(String(value))?" selected":""}>True</option></select>`;
  }
  if(type==="I"||type==="R"){
    return `<input data-setting-input="${index}" type="number" value="${esc(value)}"${item.M!==undefined?` min="${esc(item.M)}"`:""}${item.S!==undefined?` max="${esc(item.S)}"`:""} step="${type==="I"?"1":"any"}">`;
  }
  return `<input data-setting-input="${index}" type="text" value="${esc(value)}"${item.S!==undefined?` maxlength="${esc(item.S)}"`:""}>`;
}
function renderSettings(){
  const query=$("#settings-search").value.trim().toLowerCase();
  const filtered=state.settings.map((item,index)=>({item,index})).filter(({item})=>{
    const category=item.F||item.category||"";
    const text=`${item.H||item.label||""} ${item.P||item.path||""}`.toLowerCase();
    return category===state.settingsTab&&(!query||text.includes(query));
  });
  $("#settings-summary").textContent=`${filtered.length} ${state.settingsTab==="Flash/Settings"?"flash settings":"configuration items"}`;
  $("#settings-body").innerHTML=filtered.map(({item,index})=>{
    const path=item.P||item.path||"",label=item.H||item.label||path;
    const writable=!(item.W===0||item.W===false||String(item.T)==="P");
    return `<tr><td>${esc(label)}<span class="setting-path">${esc(path)}</span></td><td>${settingInput(item,index)}</td><td>${writable?`<button data-setting-set="${index}" data-write>Set</button>`:'<span class="readonly-badge">YAML</span>'}</td></tr>`;
  }).join("")||'<tr><td colspan="3">No matching settings.</td></tr>';
  updateLockUi();
}
async function refreshSettings(){
  $("#settings-summary").textContent="Loading FluidNC setting metadata…";
  try{
    const result=await jsonFetch("/api/v1/settings");
    state.settings=Array.isArray(result.data)?result.data:[];
    renderSettings();
  }catch(error){$("#settings-summary").textContent=error.message;showToast(error.message,"error");}
}
$$("[data-settings-tab]").forEach(button=>button.onclick=()=>{
  state.settingsTab=button.dataset.settingsTab;
  $$("[data-settings-tab]").forEach(item=>item.classList.toggle("active",item===button));
  renderSettings();
});
$("#settings-search").oninput=renderSettings;$("#refresh-settings").onclick=refreshSettings;
$("#settings-body").onclick=async event=>{
  const button=event.target.closest("[data-setting-set]");if(!button||button.disabled)return;
  const index=Number(button.dataset.settingSet),item=state.settings[index],input=$(`[data-setting-input="${index}"]`);
  try{
    await textFetch("/api/v1/settings",{method:"PUT",headers:writeHeaders({"Content-Type":"application/json"}),body:JSON.stringify({path:String(item.P||item.path),type:String(item.T||item.type||"S"),value:String(input.value)})});
    showToast(`${item.H||item.P} updated`,"success");await refreshSettings();
  }catch(error){showToast(error.message,"error",7000);}
};

function renderValidation(target,result){
  const labels=["signature","hash","product","board","hardware_role","compatibility","version"];
  target.innerHTML=labels.map(name=>`<span class="check ${result?.[name]===true?"pass":result?.[name]===false?"fail":""}">${esc(name.replace("_"," "))}: ${result?.[name]===true?"valid":result?.[name]===false?"invalid":"pending"}</span>`).join("");
}
function parseEnvelope(buffer){
  const bytes=new Uint8Array(buffer),view=new DataView(buffer);
  if(bytes.length<24)throw new Error("Truncated .tamsfw header");
  if(String.fromCharCode(...bytes.slice(0,7))!=="TAMSFW1")throw new Error("Wrong package magic");
  if(view.getUint16(8,true)!==1||view.getUint16(10,true)!==0)throw new Error("Unsupported package version or flags");
  const manifestLength=view.getUint32(12,true),signatureLength=view.getUint32(16,true),imageLength=view.getUint32(20,true);
  if(!manifestLength||manifestLength>4096||!signatureLength||signatureLength>128||!imageLength||24+manifestLength+signatureLength+imageLength!==bytes.length)throw new Error("Invalid package lengths");
  const manifestBytes=bytes.slice(24,24+manifestLength);
  const manifest=JSON.parse(new TextDecoder("utf-8",{fatal:true}).decode(manifestBytes));
  return {manifest,manifestBytes,imageOffset:24+manifestLength+signatureLength,imageLength,buffer};
}
async function selectPackage(kind,file){
  const target=$(`#${kind}-validation`);
  try{
    const parsed=parseEnvelope(await file.arrayBuffer());state.package[kind]=parsed;
    renderValidation(target,{product:parsed.manifest.product===(kind==="dial"?"fluiddial":"fluidnc")});
    const response=await fetch("/api/v1/firmware/packages/validate",{method:"POST",credentials:"same-origin",headers:{"Content-Type":"application/octet-stream","X-CSRF-Token":state.csrf,"X-TAMS-Target":kind},body:file});
    const text=await response.text(),result=extractJson(text);if(!response.ok)throw new Error(result.error||result.reason||"Package validation failed");
    parsed.validation=result;renderValidation(target,result.validation);
    $(`#${kind==="dial"?"dial-update-reason":"firmware-safety"}`).textContent=result.reason||"Package and target validated.";
    updateFirmwareButtons();
  }catch(error){state.package[kind]=null;target.innerHTML=`<span class="check fail">${esc(error.message)}</span>`;updateFirmwareButtons();}
}
$("#dial-package").onchange=event=>event.target.files[0]&&selectPackage("dial",event.target.files[0]);
$("#controller-package").onchange=event=>event.target.files[0]&&selectPackage("controller",event.target.files[0]);

function updateFirmwareButtons(){
  const data=state.firmware||{},dial=data.m5dial||{},dialValidation=state.package.dial?.validation,controllerValidation=state.package.controller?.validation;
  const dialDisabled=state.locked||!(dialValidation?.valid&&dialValidation?.target_exact&&data.safe&&data.trust_configured&&dial.paired&&dial.online&&!dial.ambiguous&&!data.maintenance_lock);
  const controllerDisabled=state.locked||!(controllerValidation?.valid&&data.safe&&data.trust_configured&&!data.maintenance_lock);
  $("#update-dial").dataset.serverDisabled=String(dialDisabled&&!state.locked);$("#update-dial").disabled=dialDisabled;
  $("#update-controller").dataset.serverDisabled=String(controllerDisabled&&!state.locked);$("#update-controller").disabled=controllerDisabled;
  $("#pair-dial").disabled=state.locked||!!(dial.paired&&dial.online);
}
let firmwareRefreshInFlight=null;
function refreshFirmware(){
  if(firmwareRefreshInFlight)return firmwareRefreshInFlight;
  firmwareRefreshInFlight=(async()=>{try{
    const data=await jsonFetchTimeout("/api/v1/firmware/devices",{},2500);state.firmware=data;
    const controller=data.controller||{},dial=data.m5dial||{};
    $("#controller-id").textContent=controller.device_id||"—";$("#controller-version").textContent=controller.version||"—";$("#controller-slot").textContent=controller.inactive_partition||"—";
    $("#dial-online").textContent=dial.online?"Online":dial.paired?"Offline":"Not paired";$("#dial-online").className=`pill ${dial.online?"good":""}`;
    $("#dial-id").textContent=dial.device_id||"Not paired";$("#dial-fingerprint").textContent=dial.fingerprint||"—";$("#dial-address").textContent=dial.ip?`${dial.ip} / ${dial.hardware_role||"—"}`:"—";$("#dial-version").textContent=dial.version||"—";
    $("#pair-dial").textContent=dial.paired?"Paired":"Pair M5Dial";
    $("#pair-status").textContent=dial.error||dial.health||(dial.paired?"Paired identity is stored.":"Pairing can be started while FluidDial is running normally.");
    $("#firmware-safety").textContent=data.safe?`Machine ready for firmware maintenance: ${data.safety_reason||"Idle"}`:`Updates disabled: ${data.safety_reason||"machine is active"}`;
    $("#diag-maintenance").textContent=data.maintenance_lock?"Active":"Inactive";
    if(!data.trust_configured)$("#dial-update-reason").textContent="Production signing trust is not configured.";
    updateFirmwareButtons();
    const receipts=await jsonFetchTimeout("/api/v1/firmware/receipts",{},2500).catch(()=>[]);
    const controllerReceipt=receipts.find(receipt=>receipt.target==="fluidnc_controller"),dialReceipt=receipts.find(receipt=>receipt.target==="m5dial");
    if(controllerReceipt)$("#last-controller-receipt").textContent=JSON.stringify(controllerReceipt,null,2);
    if(dialReceipt)$("#last-dial-receipt").textContent=JSON.stringify(dialReceipt,null,2);
  }catch(error){$("#firmware-safety").textContent=`Firmware service unavailable: ${error.message}`;}
  })().finally(()=>{firmwareRefreshInFlight=null;});
  return firmwareRefreshInFlight;
}
async function firmwarePoll(){
  const firmwareVisible=$("#page-firmware").classList.contains("active");
  if(firmwareVisible&&document.visibilityState==="visible")await refreshFirmware();
  setTimeout(firmwarePoll,5000);
}

async function deploy(kind){
  const pkg=state.package[kind];if(!pkg||state.locked)return;
  const progress=$(`#${kind}-progress`),stages=["Validation","Transfer","Target verification","Image verification","Reboot","Reconnect","Health","Receipt"];
  progress.innerHTML=stages.map(stage=>`<li>${stage}</li>`).join("");
  let deploymentId="";
  try{
    const start=await jsonFetch("/api/v1/firmware/deployments",{method:"POST",headers:writeHeaders({"Content-Type":"application/json"}),body:JSON.stringify({target:kind,package_id:pkg.manifest.package_id,manifest_sha256:pkg.validation.manifest_sha256})});
    deploymentId=start.deployment_id;
    const image=new Uint8Array(pkg.buffer,pkg.imageOffset,pkg.imageLength),chunkSize=start.chunk_size||4096;
    for(let offset=0;offset<image.length;offset+=chunkSize){
      progress.children[1].className="active";
      await textFetch(`/api/v1/firmware/deployments/${encodeURIComponent(deploymentId)}/chunks?offset=${offset}`,{method:"PUT",headers:writeHeaders({"Content-Type":"application/octet-stream"}),body:image.slice(offset,Math.min(offset+chunkSize,image.length))});
    }
    await jsonFetch(`/api/v1/firmware/deployments/${encodeURIComponent(deploymentId)}/commit`,{method:"POST",headers:writeHeaders()});
    for(let tries=0;tries<90;tries++){
      await new Promise(resolve=>setTimeout(resolve,1000));
      const deployment=await jsonFetch(`/api/v1/firmware/deployments/${encodeURIComponent(deploymentId)}`);
      stages.forEach((_,index)=>progress.children[index].className=index<deployment.stage_index?"done":index===deployment.stage_index?"active":"");
      if(deployment.terminal){
        if(!deployment.success)throw new Error(deployment.error||"Deployment failed");
        if(deployment.receipt_persisted===false)throw new Error("Firmware verified, but its receipt was not persisted.");
        showToast(`${kind==="dial"?"M5Dial":"FluidNC"} update complete`,"success",8000);break;
      }
    }
    await refreshFirmware();
  }catch(error){
    if(deploymentId)await jsonFetch(`/api/v1/firmware/deployments/${encodeURIComponent(deploymentId)}/abort`,{method:"POST",headers:writeHeaders()}).catch(()=>{});
    progress.insertAdjacentHTML("beforeend",`<li class="fail">${esc(error.message)}</li>`);showToast(error.message,"error",9000);
  }
}
$("#update-dial").onclick=()=>deploy("dial");$("#update-controller").onclick=()=>deploy("controller");

$("#pair-dial").onclick=async()=>{
  if(state.locked)return;
  const status=$("#pair-status"),code=$("#pair-code");
  try{
    const started=await jsonFetch("/api/v1/firmware/pair/start",{method:"POST",headers:writeHeaders()});
    code.hidden=false;code.textContent=started.comparison_code;
    status.textContent="Compare this code on the M5Dial, then press the center dial button.";
    const body=new URLSearchParams({comparison_code:started.comparison_code});
    await jsonFetch("/api/v1/firmware/pair/confirm",{method:"POST",headers:writeHeaders({"Content-Type":"application/x-www-form-urlencoded"}),body});
    for(let tries=0;tries<60;tries++){
      await new Promise(resolve=>setTimeout(resolve,1000));
      const result=await jsonFetch("/api/v1/firmware/pair/status");
      if(result.paired){status.textContent="Cryptographic pairing complete.";code.hidden=true;showToast("M5Dial paired","success");await refreshFirmware();return;}
    }
    throw new Error("Physical pairing window expired.");
  }catch(error){status.textContent=`Pairing failed: ${error.message}`;showToast(error.message,"error");}
};

function renderFiles(data){
  const files=Array.isArray(data.files)?data.files:[];
  $("#file-list").innerHTML=files.map(file=>`<div class="file-entry"><button data-open-file="${esc(file.name)}">${esc(file.name)}</button><small>${Number(file.size)<0?"folder":`${file.size} B`}</small><a class="text-link" href="/${encodeURI(file.name)}" download>Download</a></div>`).join("")||"<p>No files found.</p>";
  $("#file-space").textContent=`${data.used||"—"} used of ${data.total||"—"} (${data.occupation??"—"}%) — ${data.status||"Ok"}`;
}
async function refreshFiles(){
  try{renderFiles(await jsonFetch("/files?action=list&path=/"));}catch(error){$("#file-list").textContent=error.message;}
}
async function openFile(name){
  try{
    const text=await textFetch(`/${encodeURI(name)}`);
    state.currentFile=name;$("#editor-name").value=name;$("#file-editor").value=text;
    $("#editor-title").textContent=name;$("#editor-state").textContent="Loaded from DLC32";
    $("#download-file").disabled=false;$("#delete-file").disabled=state.locked;
  }catch(error){showToast(error.message,"error");}
}
$("#file-list").onclick=event=>{const button=event.target.closest("[data-open-file]");if(button)openFile(button.dataset.openFile);};
$("#file-editor").oninput=()=>{$("#editor-state").textContent="Unsaved browser edits";};
async function uploadNamedFile(name,blob){
  const form=new FormData();form.append("file",blob,name);form.append(`${name}S`,String(blob.size));
  await textFetch("/files",{method:"POST",headers:writeHeaders(),body:form});
}
$("#file-upload").onchange=async event=>{
  const file=event.target.files[0];if(!file||state.locked)return;
  try{await uploadNamedFile(file.name,file);showToast(`${file.name} uploaded`,"success");await refreshFiles();}catch(error){showToast(error.message,"error");}
};
$("#save-file").onclick=async()=>{
  const name=$("#editor-name").value.trim().replace(/^\/+/,"");
  if(!name||state.locked)return;
  try{await uploadNamedFile(name,new Blob([$("#file-editor").value],{type:"text/plain"}));state.currentFile=name;$("#editor-state").textContent="Saved on DLC32";showToast(`${name} saved to DLC32`,"success");await refreshFiles();}catch(error){showToast(error.message,"error");}
};
$("#download-file").onclick=()=>{
  const name=$("#editor-name").value.trim();if(!name)return;
  const link=document.createElement("a");link.href=`/${encodeURI(name)}`;link.download=name;link.click();
};
$("#delete-file").onclick=async()=>{
  const name=state.currentFile||$("#editor-name").value.trim();if(!name||state.locked)return;
  try{
    await jsonFetch(`/files?action=delete&path=/&filename=${encodeURIComponent(name)}`,{headers:writeHeaders()});
    state.currentFile="";$("#editor-name").value="";$("#file-editor").value="";$("#editor-title").textContent="File Editor";$("#editor-state").textContent="No file open";$("#download-file").disabled=true;$("#delete-file").disabled=true;
    showToast(`${name} deleted`,"success");await refreshFiles();
  }catch(error){showToast(error.message,"error");}
};
$("#refresh-files").onclick=refreshFiles;

$("#refresh-diagnostics").onclick=refreshStatus;

toolRows();renderStations($("#turret-stations"));renderStations($("#turret-control-stations"),null,null,true);
(async()=>{
  await initializeConsole();
  await refreshStatus();
  setTimeout(statusPoll,1500);
  setTimeout(firmwarePoll,5000);
})();
