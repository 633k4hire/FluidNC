"use strict";
const $=s=>document.querySelector(s),$$=s=>[...document.querySelectorAll(s)];
const state={lathe:false,fresh:false,safe:false,cReady:false,spindleReady:false,latheRaw:"",chuckRaw:"",firmware:null,package:{controller:null,dial:null}};
const esc=s=>String(s??"—").replace(/[&<>"']/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));

function setPage(name){
  $$(".page").forEach(e=>e.classList.toggle("active",e.id===`page-${name}`));
  $$("nav button").forEach(e=>e.classList.toggle("active",e.dataset.page===name));
}
$$("nav button").forEach(b=>b.onclick=()=>setPage(b.dataset.page));

async function textFetch(url,options={}){
  const response=await fetch(url,{cache:"no-store",credentials:"same-origin",...options});
  const text=await response.text();
  if(!response.ok)throw new Error(text||`${response.status} ${response.statusText}`);
  return text;
}
async function jsonFetch(url,options={}){return JSON.parse(await textFetch(url,options));}
async function command(cmd){return textFetch(`/command?cmd=${encodeURIComponent(cmd)}`);}

async function refreshAuth(){
  try{
    const auth=await jsonFetch("/login");
    const level=auth.authentication_lvl||"guest";
    $("#login-button").textContent=level==="admin"?"Administrator":"Sign in";
    return level;
  }catch(_){$("#login-button").textContent="Sign in";return "guest";}
}
$("#login-button").onclick=()=>{$("#login-error").textContent="";$("#login-dialog").showModal();$("#login-password").focus();};
$("#login-cancel").onclick=()=>$("#login-dialog").close();
$("#login-form").onsubmit=async event=>{
  event.preventDefault();$("#login-error").textContent="";
  const next=$("#login-new-password").value,confirmed=$("#login-confirm-password").value;
  if(next!==confirmed){$("#login-error").textContent="New passwords do not match.";return;}
  if(next&&(next.length<12||next.length>16)){$("#login-error").textContent="New password must be 12–16 characters.";return;}
  const body=new URLSearchParams({SUBMIT:"1",USER:$("#login-user").value,PASSWORD:$("#login-password").value});
  if(next)body.set("NEWPASSWORD",next);
  try{
    const response=await fetch("/login",{method:"POST",credentials:"same-origin",headers:{"Content-Type":"application/x-www-form-urlencoded"},body});
    const result=await response.json();if(!response.ok)throw new Error(result.status||"Sign-in failed");
    $("#login-password").value="";$("#login-new-password").value="";$("#login-confirm-password").value="";$("#login-dialog").close();await refreshAuth();await refreshStatus();await refreshFirmware();
  }catch(error){$("#login-error").textContent=error.message;}
};

function parsePairs(raw){
  const out={};
  try{
    const start=raw.indexOf("{"),end=raw.lastIndexOf("}");
    const parsed=JSON.parse(raw.slice(start,end+1));
    if(Array.isArray(parsed.data))for(const item of parsed.data||[])if(item&&item.id!==undefined)out[String(item.id).trim().toLowerCase().replace(/\s+/g,"_")]=item.value;
    for(const [key,value] of Object.entries(parsed))if(value===null||["string","number","boolean"].includes(typeof value))out[key.toLowerCase().replace(/\s+/g,"_")]=value;
    out._json=parsed;return out;
  }catch(_){}
  for(const line of raw.split(/\r?\n/)){
    const match=line.match(/^\s*([^:=]+)\s*[:=]\s*(.*?)\s*$/);
    if(match)out[match[1].trim().toLowerCase().replace(/\s+/g,"_")]=match[2].trim();
  }
  return out;
}
function first(data,...keys){for(const key of keys)if(data[key]!==undefined)return data[key];return "—";}
function truthy(value){return /^(1|true|yes|on|enabled|idle|confirmed)$/i.test(String(value));}
function number(value,fallback="—"){const n=Number(value);return Number.isFinite(n)?n.toFixed(3):fallback;}

function renderStations(container,active,target,controls=false){
  container.innerHTML="";
  for(let i=1;i<=5;i++){
    const element=document.createElement(controls?"button":"span");
    element.className=`station${String(active)===String(i)?" active":""}${String(target)===String(i)?" target":""}`;
    element.textContent=`T${i}`;
    if(controls){element.dataset.control="turret";element.dataset.tool=String(i);element.disabled=!state.safe;}
    container.append(element);
  }
}

function renderLathe(raw,chuckRaw,telemetryRaw){
  const data=parsePairs(raw),chuck=parsePairs(chuckRaw),telemetry=parsePairs(telemetryRaw)._json||{};
  const positions=telemetry.positions||{},spindle=telemetry.spindle||{},encoder=spindle.encoder||{},turret=telemetry.turret||{},execution=telemetry.execution||{},machine=telemetry.machine||{};
  Object.assign(data,{
    machine_x:positions.x?.machine,machine_z:positions.z?.machine,machine_c:positions.c?.machine,
    diameter_mode:spindle.diameter_mode,coordinate_mode:execution.coordinate_system,distance_mode:execution.distance_mode,
    feed_mode:execution.feed_mode,limits:[positions.x,positions.z,positions.c].map((p,i)=>p?.limit_active?["X","Z","C"][i]:"").filter(Boolean).join(", ")||"Clear",
    alarm:machine.alarm,state:machine.state,homed:[positions.x,positions.z,positions.c].filter(p=>p?.available).every(p=>p?.homed),
    active_tool:turret.current_station,target_tool:turret.target_station,tool_confirmed:turret.software_position_known,
    turret_state:turret.last_error,turret_sensor:turret.sensor_configured,encoder_enabled:encoder.configured,
    measured_rpm:spindle.measured_rpm,index_seen:encoder.has_index,angular_position:encoder.angular_position_revolution,
    encoder_stale:encoder.stale,encoder_fault:encoder.fault,commanded_rpm:spindle.commanded_rpm,
    spindle_state:spindle.state,spindle_mode:spindle.speed_mode,shared_chuck_enabled:spindle.shared_chuck
  });
  state.lathe=truthy(first(data,"lathe_enabled","enabled","lathe"));
  state.fresh=true;
  const mode=$("#mode-banner");
  if(!state.lathe){
    mode.hidden=false;mode.textContent="This controller did not confirm lathe support. Lathe-specific controls are hidden; Files and Diagnostics remain available.";
    $$("[data-lathe]").forEach(e=>e.hidden=true);
    $("#title").textContent="FluidNC Console";
    return;
  }
  mode.hidden=true;$$("[data-lathe]").forEach(e=>e.hidden=false);$("#title").textContent="XZA Lathe Console";
  $("#dro-x").textContent=number(first(data,"machine_x","mpos_x","x"));
  $("#dro-z").textContent=number(first(data,"machine_z","mpos_z","z"));
  $("#dro-c").textContent=number(first(data,"machine_c","mpos_c","c"));
  $("#dro-x-mode").textContent=`machine / ${first(data,"diameter_mode","x_mode")} mode`;
  $("#coordinate-mode").textContent=first(data,"coordinate_mode","distance_mode","plane");
  $("#feed-mode").textContent=first(data,"feed_mode");
  $("#limits").textContent=first(data,"limits","limit_state");
  $("#alarm").textContent=first(data,"alarm","alarm_code");
  const machineState=String(first(data,"state","machine_state"));
  $("#machine-state").textContent=machineState;
  const homed=truthy(first(data,"homed","axes_homed"));
  $("#homed").textContent=homed?"Homed":"Unhomed";$("#homed").className=`pill ${homed?"good":"danger"}`;
  const active=first(data,"active_tool","current_tool"),target=first(data,"target_tool");
  renderStations($("#turret-stations"),active,target);renderStations($("#turret-control-stations"),active,target,true);
  $("#turret-tools").textContent=`T${active} / T${target}`;
  const confirmed=truthy(first(data,"tool_confirmed","turret_confirmed"));
  $("#turret-confirmed").textContent=confirmed?"Confirmed":"Unconfirmed";
  $("#turret-state").textContent=first(data,"turret_state","tool_state");
  const sensor=truthy(first(data,"turret_sensor","sensor_configured"));
  $("#turret-sensor").textContent=sensor?"Installed":"Not installed";
  $("#turret-warning").hidden=sensor;
  const encoderReady=truthy(first(data,"encoder_enabled","encoder_enable"));
  const threading=truthy(first(data,"threading_enabled","enable_threading"));
  $("#encoder-state").textContent=encoderReady?"Enabled / verify health":"Not commissioned";
  $("#encoder-state").className=`pill ${encoderReady?"":"danger"}`;
  $("#encoder-rpm").textContent=first(data,"measured_rpm","encoder_rpm");
  $("#encoder-phase").textContent=`${first(data,"index_seen","encoder_index")} / ${first(data,"angular_position","encoder_phase")}`;
  $("#encoder-freshness").textContent=first(data,"encoder_stale","encoder_fault","not commissioned");
  $("#threading").textContent=threading?"Enabled — verify commissioning":"Disabled";
  $("#spindle-command").textContent=`${first(data,"commanded_rpm","spindle_rpm")} RPM / ${first(data,"spindle_direction","spindle_state")}`;
  $("#spindle-measured").textContent=encoderReady?`${first(data,"measured_rpm","encoder_rpm")} RPM`:"Not commissioned";
  $("#spindle-mode").textContent=first(data,"spindle_mode","css_mode");
  $("#c-position").textContent=first(data,"angular_position","c_axis_position");
  const owner=first(chuck,"owner","shared_chuck_owner","mode")!=="—"?first(chuck,"owner","shared_chuck_owner","mode"):spindle.mode;
  $("#chuck-owner").textContent=`Owner: ${owner}`;
  state.cReady=encoderReady&&/c.?position/i.test(owner);
  state.spindleReady=!truthy(first(data,"shared_chuck_enabled"))||/spindle/i.test(owner);
  $("#c-capability").textContent=encoderReady?"position feedback present; verify health":"angular positioning not commissioned";
  state.safe=/^idle$/i.test(machineState)&&!threading;
  updateControlState();
}

function updateControlState(){
  $$("[data-control]").forEach(e=>e.disabled=!state.safe);
  $$('[data-control="jog"][data-axis="C"]').forEach(e=>e.disabled=!state.safe||!state.cReady);
  $$('[data-control="spindle"]').forEach(e=>e.disabled=!state.safe||!state.spindleReady);
  $$('[data-control="spindle-stop"]').forEach(e=>e.disabled=!state.fresh);
  $("#control-disabled-reason").textContent=state.safe?"Server safety gates remain authoritative.":"Controls require fresh Idle state, empty planner, spindle off, chuck/turret idle, and no pending action.";
}

async function refreshStatus(){
  try{
    const [lathe,chuck,telemetry]=await Promise.all([command("[ESP421]"),command("[ESP426]").catch(e=>`error=${e.message}`),command("[ESP425]")]);
    state.latheRaw=lathe;state.chuckRaw=chuck;renderLathe(lathe,chuck,telemetry);
    $("#connection-dot").className="dot online";$("#connection-label").textContent="Controller online";
    $("#raw-reports").textContent=`[ESP421]\n${lathe}\n\n[ESP426]\n${chuck}\n\n[ESP425]\n${telemetry}`;
  }catch(error){
    state.fresh=false;state.safe=false;updateControlState();
    $("#connection-dot").className="dot offline";$("#connection-label").textContent="Controller unavailable";
    $("#machine-state").textContent="Stale";
  }
}

async function confirmAction(title,message){
  const dialog=$("#confirm-dialog");$("#confirm-title").textContent=title;$("#confirm-message").textContent=message;
  if(!dialog.showModal)return confirm(message);
  dialog.showModal();return new Promise(resolve=>dialog.addEventListener("close",()=>resolve(dialog.returnValue==="confirm"),{once:true}));
}

document.addEventListener("click",async event=>{
  const button=event.target.closest("[data-control]");if(!button||button.disabled)return;
  const type=button.dataset.control;
  let body={};
  if(type==="jog")body={axis:button.dataset.axis,direction:Number(button.dataset.direction),increment:Number($("#jog-increment").value),feed:Number($("#jog-feed").value)};
  if(type==="spindle")body={direction:button.dataset.direction,rpm:Number($("#spindle-rpm").value)};
  if(type==="turret")body={tool:Number(button.dataset.tool)};
  if(type==="chuck")body={mode:button.dataset.mode};
  const message=type==="turret"?`Select T${body.tool}? The command will not repeat automatically.`:`Send guarded ${type} request?`;
  if(!await confirmAction("Confirm lathe action",message))return;
  try{await textFetch(`/api/v1/lathe/${type}`,{method:"POST",headers:{"Content-Type":"application/json","X-CSRF-Token":csrfToken()},body:JSON.stringify(body)});}catch(error){alert(error.message);}
});
$("#confirm-station").onclick=async()=>{
  const tool=prompt("Enter the visually inspected physical station (1–5):");if(!/^[1-5]$/.test(tool||""))return;
  if(!await confirmAction("Confirm physical station",`I visually inspected the turret and confirm it is physically locked at T${tool}.`))return;
  try{await textFetch("/api/v1/lathe/turret/confirm",{method:"POST",headers:{"Content-Type":"application/json","X-CSRF-Token":csrfToken()},body:JSON.stringify({tool:Number(tool),visual_inspection:true})});}catch(error){alert(error.message);}
};

function csrfToken(){return sessionStorage.getItem("tams-csrf")||"";}
function renderValidation(target,result){
  const labels=["signature","hash","product","board","hardware_role","compatibility","version"];
  target.innerHTML=labels.map(name=>`<span class="check ${result?.[name]===true?"pass":result?.[name]===false?"fail":""}">${esc(name.replace("_"," "))}: ${result?.[name]===true?"valid":result?.[name]===false?"invalid":"pending"}</span>`).join("");
}
function parseEnvelope(buffer){
  const bytes=new Uint8Array(buffer),view=new DataView(buffer);
  if(bytes.length<24)throw new Error("Truncated .tamsfw header");
  const magic=String.fromCharCode(...bytes.slice(0,7));if(magic!=="TAMSFW1")throw new Error("Wrong package magic");
  if(view.getUint16(8,true)!==1||view.getUint16(10,true)!==0)throw new Error("Unsupported package version or flags");
  const ml=view.getUint32(12,true),sl=view.getUint32(16,true),il=view.getUint32(20,true);
  if(!ml||ml>4096||!sl||sl>128||!il||24+ml+sl+il!==bytes.length)throw new Error("Invalid package lengths");
  const raw=bytes.slice(24,24+ml),manifest=JSON.parse(new TextDecoder("utf-8",{fatal:true}).decode(raw));
  return {manifest,manifestBytes:raw,signature:bytes.slice(24+ml,24+ml+sl),imageOffset:24+ml+sl,imageLength:il,buffer};
}
async function selectPackage(kind,file){
  const target=$(`#${kind}-validation`),button=$(`#update-${kind}`);
  try{
    const parsed=parseEnvelope(await file.arrayBuffer());
    state.package[kind]=parsed;renderValidation(target,{hash:null,signature:null,product:parsed.manifest.product==="fluiddial"||kind==="controller",board:null,hardware_role:null,compatibility:null,version:null});
    const response=await fetch("/api/v1/firmware/packages/validate",{method:"POST",credentials:"same-origin",headers:{"Content-Type":"application/octet-stream","X-CSRF-Token":csrfToken(),"X-TAMS-Target":kind},body:file});
    const result=await response.json();renderValidation(target,result.validation);parsed.validation=result;
    button.disabled=!(result.valid&&result.safe&&result.target_exact);
    $(`#${kind==="dial"?"dial-update-reason":"firmware-safety"}`).textContent=result.reason||"Package and target validated.";
  }catch(error){state.package[kind]=null;button.disabled=true;target.innerHTML=`<span class="check fail">${esc(error.message)}</span>`;}
}
$("#dial-package").onchange=e=>e.target.files[0]&&selectPackage("dial",e.target.files[0]);
$("#controller-package").onchange=e=>e.target.files[0]&&selectPackage("controller",e.target.files[0]);

async function refreshFirmware(){
  renderValidation($("#dial-validation"));renderValidation($("#controller-validation"));
  try{
    const data=await jsonFetch("/api/v1/firmware/devices");state.firmware=data;
    if(data.csrf_token)sessionStorage.setItem("tams-csrf",data.csrf_token);
    const c=data.controller||{},d=data.m5dial||{};
    $("#controller-id").textContent=c.device_id||"—";$("#controller-version").textContent=c.version||"—";$("#controller-slot").textContent=c.inactive_partition||"—";
    $("#dial-online").textContent=d.online?"Online":d.paired?"Offline":"Not paired";$("#dial-online").className=`pill ${d.online?"good":""}`;
    $("#dial-id").textContent=d.device_id||"Not paired";$("#dial-fingerprint").textContent=d.fingerprint||"—";$("#dial-address").textContent=d.ip?`${d.ip} / ${d.hardware_role||"—"}`:"—";$("#dial-version").textContent=d.version||"—";
    $("#pair-dial").disabled=!!(d.paired&&d.online);$("#pair-dial").textContent=d.paired?"Paired":"Pair M5Dial";
    $("#pair-status").textContent=d.error||d.health||(d.paired?"Paired identity is stored.":"Open the OTA scene on the M5Dial to create a physical pairing window.");
    const passwordReady=data.admin_password_hardened!==false;
    $("#firmware-safety").textContent=!passwordReady?"Updates and typed controls disabled: replace the default administrator password.":data.safe?`Machine safe for maintenance: ${data.safety_reason||"Idle"}`:`Updates disabled: ${data.safety_reason||"unsafe or stale machine state"}`;
    const dialValidated=state.package.dial?.validation;
    $("#update-dial").disabled=!(passwordReady&&dialValidated?.valid&&dialValidated?.target_exact&&data.safe&&data.trust_configured&&d.paired&&d.online&&!d.ambiguous&&!data.maintenance_lock);
    const controllerValidated=state.package.controller?.validation;
    $("#update-controller").disabled=!(passwordReady&&controllerValidated?.valid&&data.safe&&data.trust_configured&&!data.maintenance_lock);
    if(!data.trust_configured)$("#dial-update-reason").textContent="Updates disabled: production signing trust is not configured in this build.";
    try{
      const receipts=await jsonFetch("/api/v1/firmware/receipts");
      const controllerReceipt=receipts.find(receipt=>receipt.target==="fluidnc_controller");
      const dialReceipt=receipts.find(receipt=>receipt.target==="m5dial");
      if(controllerReceipt)$("#last-controller-receipt").textContent=JSON.stringify(controllerReceipt,null,2);
      if(dialReceipt)$("#last-dial-receipt").textContent=JSON.stringify(dialReceipt,null,2);
    }catch(_){}
  }catch(error){$("#firmware-safety").textContent=`Firmware service unavailable: ${error.message}`;}
}

async function deploy(kind){
  const pkg=state.package[kind];if(!pkg)return;
  if(!await confirmAction(`Update ${kind==="dial"?"M5Dial":"FluidNC"}`,`Install signed version ${pkg.manifest.version}? Machine commands remain locked for the deployment.`))return;
  const progress=$(`#${kind}-progress`),stages=["Validation","Transfer","Target verification","Image verification","Reboot","Reconnect","Health","Receipt"];
  progress.innerHTML=stages.map(s=>`<li>${s}</li>`).join("");
  let deploymentId="";
  try{
    const start=await jsonFetch("/api/v1/firmware/deployments",{method:"POST",headers:{"Content-Type":"application/json","X-CSRF-Token":csrfToken()},body:JSON.stringify({target:kind,package_id:pkg.manifest.package_id,manifest_sha256:pkg.validation.manifest_sha256})});
    deploymentId=start.deployment_id;
    const image=new Uint8Array(pkg.buffer,pkg.imageOffset,pkg.imageLength),chunkSize=start.chunk_size||4096;
    for(let offset=0;offset<image.length;offset+=chunkSize){
      progress.children[1].className="active";
      const chunk=image.slice(offset,Math.min(offset+chunkSize,image.length));
      await textFetch(`/api/v1/firmware/deployments/${encodeURIComponent(start.deployment_id)}/chunks?offset=${offset}`,{method:"PUT",headers:{"Content-Type":"application/octet-stream","X-CSRF-Token":csrfToken()},body:chunk});
    }
    await jsonFetch(`/api/v1/firmware/deployments/${encodeURIComponent(start.deployment_id)}/commit`,{method:"POST",headers:{"X-CSRF-Token":csrfToken()}});
    for(let tries=0;tries<90;tries++){
      await new Promise(r=>setTimeout(r,1000));
      const status=await jsonFetch(`/api/v1/firmware/deployments/${encodeURIComponent(start.deployment_id)}`);
      stages.forEach((_,i)=>progress.children[i].className=i<status.stage_index?"done":i===status.stage_index?"active":"");
      if(status.terminal){
        if(!status.success)throw new Error(status.error||"Deployment failed");
        if(status.receipt_persisted===false)throw new Error("Firmware verified, but the deployment receipt could not be persisted.");
        break;
      }
    }
    await refreshFirmware();
  }catch(error){
    if(deploymentId){try{await jsonFetch(`/api/v1/firmware/deployments/${encodeURIComponent(deploymentId)}/abort`,{method:"POST",headers:{"X-CSRF-Token":csrfToken()}});}catch(_){}}
    progress.insertAdjacentHTML("beforeend",`<li class="fail">${esc(error.message)}</li>`);
  }
}
$("#update-dial").onclick=()=>deploy("dial");$("#update-controller").onclick=()=>deploy("controller");

$("#pair-dial").onclick=async()=>{
  const status=$("#pair-status");
  try{
    const started=await jsonFetch("/api/v1/firmware/pair/start",{method:"POST",headers:{"X-CSRF-Token":csrfToken()}});
    status.textContent=`Comparison code ${started.comparison_code}. Verify it on both screens.`;
    if(!await confirmAction("Pair exact M5Dial",`Verify code ${started.comparison_code} on the M5Dial. Continue only if both screens match.`))return;
    const body=new URLSearchParams({comparison_code:started.comparison_code});
    await jsonFetch("/api/v1/firmware/pair/confirm",{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded","X-CSRF-Token":csrfToken()},body});
    status.textContent="Press green on the M5Dial to physically confirm pairing.";
    for(let tries=0;tries<60;tries++){
      await new Promise(r=>setTimeout(r,1000));
      const result=await jsonFetch("/api/v1/firmware/pair/status");
      if(result.paired){status.textContent="Cryptographic pairing complete.";await refreshFirmware();return;}
    }
    throw new Error("Physical pairing window expired.");
  }catch(error){status.textContent=`Pairing failed: ${error.message}`;}
};

function toolRows(){
  $("#tool-table").innerHTML=[1,2,3,4,5].map(t=>`<tr data-tool="${t}"><td>T${t}${t===5?" / probe":""}</td>${["gx","gz","wx","wz","nr","o"].map(k=>`<td><input data-field="${k}" type="number" step="0.001"></td>`).join("")}<td><button data-save-tool="${t}">Save</button> <button data-touch-tool="${t}">Touch-off</button></td></tr>`).join("");
}
$("#refresh-tools").onclick=async()=>{try{const raw=await command("[ESP421]");$("#raw-reports").textContent=raw;}catch(e){alert(e.message);}};
$("#tool-table").onclick=async event=>{
  const save=event.target.closest("[data-save-tool]"),touch=event.target.closest("[data-touch-tool]");if(!save&&!touch)return;
  const tool=Number((save||touch).dataset.saveTool||(save||touch).dataset.touchTool),row=event.target.closest("tr");
  try{
    if(save){
      const value=name=>Number(row.querySelector(`[data-field="${name}"]`).value||0);
      const body={tool,geometry_x:value("gx"),geometry_z:value("gz"),wear_x:value("wx"),wear_z:value("wz"),nose_radius:value("nr"),orientation:value("o")};
      if(!await confirmAction(`Save T${tool}`,`Write the entered T${tool} geometry and wear values using guarded ESP422 semantics?`))return;
      await textFetch("/api/v1/lathe/tool",{method:"POST",headers:{"Content-Type":"application/json","X-CSRF-Token":csrfToken()},body:JSON.stringify(body)});
    }else{
      const axis=(prompt("Touch-off axis: X or Z","Z")||"").toUpperCase();if(!/^[XZ]$/.test(axis))return;
      const machine=Number(prompt(`Current machine ${axis} coordinate (mm):`));const reference=Number(prompt(`Known ${axis} reference coordinate (mm):`));
      if(!Number.isFinite(machine)||!Number.isFinite(reference))return;
      const mode=axis==="X"?(prompt("X interpretation: diameter or radius","diameter")||"").toLowerCase():"radius";
      if(!["diameter","radius"].includes(mode))return;
      if(!await confirmAction(`Touch off T${tool} ${axis}`,`Write ${axis} offset from machine ${machine} to reference ${reference}. This does not run a probe cycle.`))return;
      await textFetch("/api/v1/lathe/touch-off",{method:"POST",headers:{"Content-Type":"application/json","X-CSRF-Token":csrfToken()},body:JSON.stringify({tool,axis,machine,reference,mode})});
    }
  }catch(error){alert(error.message);}
};
$("#refresh-diagnostics").onclick=()=>refreshStatus();
$("#refresh-files").onclick=async()=>{try{$("#files-output").textContent=await textFetch("/files?action=list&path=/");}catch(e){$("#files-output").textContent=e.message;}};
$("#legacy-command-form").onsubmit=async e=>{e.preventDefault();try{$("#legacy-output").textContent=await command($("#legacy-command").value);}catch(error){$("#legacy-output").textContent=error.message;}};

toolRows();renderStations($("#turret-stations"));renderStations($("#turret-control-stations"),null,null,true);
refreshAuth();refreshStatus();refreshFirmware();setInterval(refreshStatus,1500);setInterval(refreshFirmware,5000);
