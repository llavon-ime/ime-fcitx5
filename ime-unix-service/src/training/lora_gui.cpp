#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <notify.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

volatile sig_atomic_t stop_requested = 0;
void request_stop(int) { stop_requested = 1; }

// Records shown per page; the page script steps its offset by the same value.
constexpr int kRecordsPerPage = 20;

constexpr std::string_view page = R"LLAVON(<!doctype html>
<html lang="zh-Hant"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="referrer" content="no-referrer"><link rel="icon" type="image/png" href="@@LOGO@@"><title>拉風輸入法・個人化訓練</title>
<style>
:root{
  color:#26221e;background:#f7f4ee;
  font-family:system-ui,-apple-system,"Noto Sans TC","PingFang TC","Microsoft JhengHei",sans-serif;
  font-synthesis:none;text-rendering:optimizeLegibility;line-height:1.5;
  --ink:#26221e;--muted:#665e55;--paper:#fff;--line:#ded7cd;--line-strong:#a99d90;
  --accent:#a34825;--accent-dark:#7b321b;--accent-soft:#f7e7dc;
  --warning:#9b572d;--warning-soft:#fff5eb;--danger:#a6422e;--danger-soft:#fff0ed;
}
*{box-sizing:border-box}
html{background:#f7f4ee}
body{min-width:320px;min-height:100vh;margin:0;background:#f7f4ee}
button,input,textarea,select{font:inherit}
.site-shell{min-height:100vh}
.topbar{min-height:72px;display:flex;align-items:center;justify-content:space-between;gap:24px;padding:0 max(24px,calc((100vw - 1040px)/2));border-bottom:1px solid var(--line);background:rgba(255,253,249,.94)}
.brand{min-width:0;display:inline-flex;align-items:center;gap:11px}
.brand-logo{width:44px;height:44px;flex:0 0 auto;border:1px solid var(--line-strong);border-radius:11px;background:var(--paper);object-fit:cover}
.brand > span{min-width:0;display:flex;flex-direction:column;line-height:1.2}
.brand strong{font-size:15px;letter-spacing:.02em}
.brand small{margin-top:3px;color:var(--muted);font-size:11px}
.top-note{flex:0 0 auto;padding:6px 9px;border:1px solid var(--line);border-radius:7px;background:var(--paper);color:var(--muted);font-size:10px;font-weight:700}
.topbar-actions{display:flex;align-items:center;gap:9px}
.page-content{width:min(820px,calc(100% - 40px));margin:0 auto;padding:40px 0 64px}
.page-heading{margin-bottom:22px}
.page-heading h1{margin:0;font-size:clamp(28px,4vw,38px);line-height:1.25;letter-spacing:-.035em}
.page-heading p{margin:8px 0 0;color:var(--muted);font-size:14px}
.form-card{min-width:0;overflow:hidden;border:1px solid var(--line);border-radius:14px;background:var(--paper);box-shadow:0 12px 32px rgba(54,42,32,.075);margin-bottom:14px}
.field-group{padding:22px 24px;border-bottom:1px solid var(--line)}
.field-group:last-child{border-bottom:0}
.field-label-row{display:flex;align-items:flex-end;justify-content:space-between;gap:18px;margin-bottom:13px}
.field-label{display:inline-flex;align-items:center;gap:9px;font-size:14px;font-weight:800}
.field-index{width:23px;height:23px;display:inline-grid;place-items:center;flex:0 0 auto;border-radius:6px;background:var(--accent);color:#fff;font-size:10px;font-weight:800}
.field-label-row small,.field-label-row > span:not(.field-label){color:var(--muted);font-size:10px}
.field-hint{margin:9px 1px 0;color:var(--muted);font-size:10px;line-height:1.6}
.toolbar{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.row{display:flex;align-items:center;gap:9px;flex-wrap:wrap;margin:0}
.notice{display:flex;gap:10px;margin-bottom:18px;padding:13px 15px;border:1px solid var(--line);border-left:4px solid var(--accent);border-radius:10px;background:var(--paper);font-size:12px;white-space:pre-wrap}
.notice.error{border-left-color:var(--danger);background:var(--danger-soft);color:#7b2f21}
button{min-height:40px;padding:0 15px;border:1px solid var(--line-strong);border-radius:8px;background:var(--paper);color:var(--ink);font-size:12px;font-weight:800;cursor:pointer;transition:background 150ms,border-color 150ms}
button:disabled{cursor:not-allowed;opacity:.55}
button.primary{border:0;background:var(--accent);color:#fff}
button.primary:not(:disabled):hover{background:var(--accent-dark)}
button.ghost:not(:disabled):hover{border-color:var(--accent);color:var(--accent-dark)}
button.danger{color:var(--danger)}
button.tiny{min-height:30px;padding:0 10px;font-size:11px;border-radius:7px}
select{min-height:36px;padding:0 9px;border:1px solid var(--line-strong);border-radius:8px;outline:none;background:var(--paper);color:var(--ink);font-size:12px;font-weight:700}
input:not([type=checkbox]){min-height:44px;padding:0 13px;border:1px solid var(--line-strong);border-radius:9px;outline:none;background:var(--paper);color:var(--ink);font-size:15px;font-weight:600;transition:border-color 150ms,box-shadow 150ms}
select:focus-visible,input:not([type=checkbox]):focus{border-color:var(--accent);box-shadow:0 0 0 3px rgba(163,72,37,.15)}
.statusline{display:flex;align-items:center;gap:10px;flex-wrap:wrap;font-size:12px;margin-bottom:14px}
.statusline .revision{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:10px;color:var(--muted);overflow-wrap:anywhere}
.records{display:grid;gap:10px}
.record{border:1px solid var(--line);border-radius:12px;background:var(--paper);padding:12px 14px;display:grid;gap:8px}
.record.selected{border-color:#d9b9a6;box-shadow:0 0 0 3px rgba(163,72,37,.10)}
.record-head{display:flex;align-items:center;gap:8px;flex-wrap:wrap;color:var(--muted);font-size:10px}
.record-head .time{font-weight:800;color:#55463c}
.record-head .actions{margin-left:auto;display:flex;gap:6px}
.align{padding:3px 8px;border-radius:999px;background:var(--accent-soft);color:var(--accent-dark);font-size:9px;font-weight:800}
.align.partial{background:var(--warning-soft);color:var(--warning)}
.sentence{margin:0;color:var(--ink);font-family:"Noto Serif TC","Songti TC","PMingLiU","Noto Serif CJK TC",ui-serif,serif;font-size:clamp(18px,3.1vw,22px);line-height:1.95;overflow-wrap:anywhere}
.sentence .context{color:var(--muted);white-space:pre-wrap}
.answer{padding:.62em 7px 4px;border-radius:6px;background:var(--accent-soft);color:var(--accent-dark);box-decoration-break:clone;-webkit-box-decoration-break:clone;font-weight:800}
.syllable{position:relative;display:inline-block;vertical-align:bottom;line-height:1.15}
.syllable .reading{position:absolute;left:50%;bottom:100%;transform:translateX(-50%);font-family:inherit;font-size:.36em;font-weight:800;letter-spacing:.01em;color:var(--accent-dark);white-space:nowrap;margin-bottom:1px}
.syllable .character{font-size:1em}
.syllable.manual{background:#eed3c1;border-radius:5px}
.syllable .reading.unresolved{color:var(--warning)}
.record-foot{display:flex;align-items:baseline;gap:10px;flex-wrap:wrap;padding-top:8px;border-top:1px solid #f1e8e2;font-size:10px;color:var(--muted)}
.record-foot .readings{flex:1 1 auto;min-width:0;overflow-wrap:anywhere}
.record-foot .readings strong{margin-left:6px;color:#55463c;font-weight:700}
.revised{color:var(--accent);font-weight:800;letter-spacing:.04em}
.checkbox{width:16px;height:16px;accent-color:var(--accent)}
.chip{padding:4px 8px;border-radius:999px;background:var(--warning-soft);color:var(--warning);font-size:9px;font-weight:800}
.chip.trained{background:var(--accent-soft);color:var(--accent-dark)}
.chip.excluded{background:#eee8e1;color:var(--muted)}
.tag{padding:4px 8px;border-radius:999px;background:#f2ede7;color:var(--muted);font-size:9px;font-weight:800}
.tag.error{background:var(--danger-soft);color:var(--danger)}
.mono{font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
.options{display:grid;grid-template-columns:repeat(auto-fill,minmax(168px,1fr));gap:14px 16px;margin-bottom:16px}
.field{display:flex;flex-direction:column;gap:6px}
.field span{color:#554e47;font-size:11px;font-weight:800}
.field input,.field select{min-width:0}
.switch{display:flex;align-items:center;gap:9px;align-self:end;padding-bottom:11px;font-size:12px;font-weight:700}
.switch input{width:16px;height:16px;accent-color:var(--accent)}
.estimate{display:flex;align-items:center;gap:14px;flex-wrap:wrap;margin:0 0 16px;color:var(--muted);font-size:11px}
progress{width:100%;height:6px;accent-color:var(--accent);border:none;border-radius:999px}
.hint{margin:9px 1px 0;color:var(--muted);font-size:10px;line-height:1.6}
.log{margin-top:14px;padding:13px 15px;border:1px solid var(--line);border-radius:10px;background:#faf8f4;color:#554e47;font-size:11px;max-height:14rem;overflow:auto;white-space:pre-wrap;overflow-wrap:anywhere}
.run{display:grid;gap:6px;margin-bottom:8px;padding:11px 13px;border:1px solid var(--line);border-radius:10px;background:var(--paper)}
.run-top{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.run-top strong{font-size:12px}
.run-top button{margin-left:auto}
.tagrow{display:flex;gap:6px;flex-wrap:wrap}
.path{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:10px;color:var(--muted);overflow-wrap:anywhere}
.empty{margin:0;color:var(--muted);font-size:12px}
</style>
<div class="site-shell">
<header class="topbar">
  <span class="brand">
    <img class="brand-logo" src="@@LOGO@@" alt="">
    <span><strong>拉風輸入法</strong><small>個人化訓練</small></span>
  </span>
  <span class="topbar-actions">
    <button id="reload-records" class="ghost tiny">重新整理</button>
    <span class="top-note">資料只留在本機</span>
  </span>
</header>
<main class="page-content">
  <div class="page-heading">
    <h1>個人化訓練</h1>
    <p>收集開關仍在輸入法設定中。基礎模型 <span class="mono">tony65535/llavon-ime-llama-250m</span>（約 1 GB，CC-BY-NC-4.0）。</p>
  </div>
  <div id="message" class="notice">連線中…</div>
  <section class="form-card">
    <div class="field-group">
      <div class="field-label-row">
        <span class="field-label"><span class="field-index">01</span>提交紀錄</span>
        <div class="toolbar">
          <label for="record-state">顯示</label>
          <select id="record-state"><option value="pending">待訓練</option><option value="excluded">已排除</option><option value="trained">已訓練</option></select>
          <button id="select-all" class="ghost tiny">全選</button>
          <button id="clear-all" class="ghost tiny">全部取消</button>
          <button id="previous" class="ghost tiny">上一頁</button>
          <button id="next" class="ghost tiny">下一頁</button>
          <span id="page-summary"></span>
          <span id="selection-summary"></span>
        </div>
      </div>
      <div id="records" class="records"></div>
    </div>
  </section>
  <section class="form-card">
    <div class="field-group">
      <div class="field-label-row">
        <span class="field-label"><span class="field-index">02</span>基礎模型</span>
      </div>
      <div id="model-status" class="statusline"></div>
      <div class="row"><button id="check" class="ghost">檢查更新</button><button id="fetch" class="primary">下載／更新模型</button></div>
      <p class="field-hint">下載後才能開始訓練；訓練不會自動替換目前使用的模型。</p>
    </div>
  </section>
  <section class="form-card">
    <div class="field-group">
      <div class="field-label-row">
        <span class="field-label"><span class="field-index">03</span>訓練設定</span>
      </div>
      <div id="training-options" class="options"></div>
      <div class="estimate"><span id="estimated-steps">預計 steps：0</span><progress id="progress" max="100" style="display:none"></progress></div>
      <div class="row"><button id="install-trainer" class="ghost">安裝／更新 LoRA Trainer</button><button id="cancel" class="ghost">取消目前工作</button><button id="train" class="primary">開始訓練 →</button></div>
      <small id="trainer-status" class="hint"></small>
      <pre id="log" class="log"></pre>
    </div>
  </section>
  <section class="form-card">
    <div class="field-group">
      <div class="field-label-row">
        <span class="field-label"><span class="field-index">04</span>訓練歷程</span>
      </div>
      <div id="runs"></div>
    </div>
  </section>
</main>
</div>
<script>
const token = location.hash.slice(1) || sessionStorage.getItem('llavon-token');
if (location.hash) { sessionStorage.setItem('llavon-token', token); history.replaceState(null, '', '/'); }
const message = document.getElementById('message');
let recordOffset=0;
const PAGE_SIZE=20;
const selectedIds=new Map();
let pendingIds=[];
let reviewedIds=null;
let readingsTable={};
let activeModelPath='';
function updateEstimate(){
  const count=pendingIds.filter(id=>selectedIds.get(id)!==false).length;
  document.getElementById('selection-summary').textContent=`已選 ${count} / ${pendingIds.length} 筆`;
  const values=['batch-size','gradient-accumulation','epochs','max-steps'].map(name=>Number(document.getElementById(name).value));
  if(values.every(Number.isInteger)&&values[0]>0&&values[1]>0&&values[2]>0&&(values[3]===-1||values[3]>0)){
    const epochs=Math.ceil(Math.ceil(count/values[0])/values[1])*values[2];
    document.getElementById('estimated-steps').textContent='預計 steps：'+(values[3]>0?Math.min(epochs,values[3]):epochs);
  }
}
const fields=[['rank','LoRA rank','8'],['alpha','LoRA alpha','16'],['dropout','LoRA dropout','0'],
  ['batch-size','Batch size','1'],['gradient-accumulation','Gradient accumulation','1'],
  ['epochs','Epochs','5'],['max-steps','Max steps','-1'],['learning-rate','Learning rate','0.0001'],
  ['weight-decay','Weight decay','0'],['warmup-steps','Warmup steps','0'],
  ['max-grad-norm','Max gradient norm','1'],['save-every','Save every','0'],['seed','Seed','42'],
  ['max-seq-length','Max sequence length','384'],['target-modules','Target modules','q_proj,v_proj']];
const optionsView=document.getElementById('training-options');
for(const [name,label,value] of fields){
  const field=document.createElement('label');field.className='field';
  const caption=document.createElement('span');caption.textContent=label;
  const input=document.createElement('input');input.id=name;input.value=value;
  field.append(caption,input);optionsView.append(field);
  input.oninput=updateEstimate;
}
for(const [name,label,choices] of [['device','Device',['auto','cuda','cpu']],['dtype','DType',['float32','bfloat16']]]){
  const field=document.createElement('label');field.className='field';
  const caption=document.createElement('span');caption.textContent=label;
  const select=document.createElement('select');select.id=name;
  for(const choice of choices){const item=document.createElement('option');item.textContent=choice;select.append(item);}
  field.append(caption,select);optionsView.append(field);
}
const shuffleLabel=document.createElement('label');shuffleLabel.className='switch';
const shuffle=document.createElement('input');shuffle.type='checkbox';shuffle.id='shuffle';shuffle.checked=true;
shuffleLabel.append(shuffle,document.createTextNode('Shuffle training data'));optionsView.append(shuffleLabel);
async function api(path, body) {
  const options = {headers:{'X-Llavon-Token':token}};
  if (body !== undefined) {options.method='POST';options.headers['Content-Type']='application/json';options.body=JSON.stringify(body);}
  const response = await fetch('/api/'+path, options);
  const result = await response.json();
  if (!response.ok) throw Error(result.error || '請求失敗');
  return result;
}
function stateChip(state){
  const chip=document.createElement('span');chip.className='chip '+(state==='pending'?'':state);
  chip.textContent={pending:'待訓練',excluded:'已排除',trained:'已訓練'}[state]||state;
  return chip;
}
function tableReadings(character){
  const known=readingsTable[character];
  return Array.isArray(known)?known:[];
}
function readingSequence(item){
  const characters=Array.from(item.answer||'');
  const readings=item.readings||[];
  return characters.map((character,index)=>{
    const reading=readings[index]||tableReadings(character)[0];
    return reading||'待選';
  }).join('　');
}
function composed(item){
  const sentence=document.createElement('p');sentence.className='sentence';
  if(item.context){
    const context=document.createElement('span');context.className='context';
    context.textContent=item.context;sentence.append(context);
  }
  const answer=document.createElement('span');answer.className='answer';
  const characters=Array.from(item.answer||'');
  const readings=item.readings||[];
  const manual=item.manual||[];
  characters.forEach((character,index)=>{
    const known=tableReadings(character);
    const reading=readings[index]||known[0];
    const syllable=document.createElement('span');syllable.className='syllable';
    if(manual[index])syllable.classList.add('manual');
    const annotation=document.createElement('span');annotation.className='reading';
    annotation.textContent=reading||'待選';
    // The character table decides whether the recorded reading is plausible;
    // a reading outside it is flagged instead of silently rendered.
    const normalized=value=>(value||'').replace(/\s+$/,'');
    if(reading&&known.length&&!known.some(item=>normalized(item)===normalized(reading)))annotation.classList.add('unresolved');
    if(known.length)syllable.title=character+'：'+known.join('、');
    const text=document.createElement('span');text.className='character';text.textContent=character;
    syllable.append(annotation,text);answer.append(syllable);
  });
  sentence.append(answer);
  return sentence;
}
function recordCard(item, viewState){
  const card=document.createElement('article');card.className='record';
  const checked=selectedIds.get(item.id)!==false;
  if(viewState==='pending'&&checked)card.classList.add('selected');
  const head=document.createElement('div');head.className='record-head';
  if(viewState==='pending'){
    const box=document.createElement('input');box.type='checkbox';box.className='checkbox';box.checked=checked;
    box.onchange=()=>{selectedIds.set(item.id,box.checked);card.classList.toggle('selected',box.checked);updateEstimate();};
    head.append(box);
  }
  const time=document.createElement('span');time.className='time';time.textContent=item.committed_at;
  head.append(time,stateChip(viewState));
  const aligned=(item.readings||[]).length>=Array.from(item.answer||'').length;
  const alignment=document.createElement('span');alignment.className='align'+(aligned?'':' partial');
  alignment.textContent=aligned?'已完整對齊':'部分對齊';head.append(alignment);
  const actions=document.createElement('div');actions.className='actions';
  const buttons=viewState==='pending'?[['排除','exclude'],['刪除','delete']]:[['刪除','delete']];
  for(const [label,action] of buttons){
    const button=document.createElement('button');
    button.className='ghost tiny'+(action==='delete'?' danger':'');button.textContent=label;
    button.onclick=async()=>{if(action==='delete'&&!confirm('確定刪除這筆紀錄？'))return;
      selectedIds.delete(item.id);await act('records/'+item.id+'/'+action,{});};
    actions.append(button);
  }
  head.append(actions);
  const foot=document.createElement('div');foot.className='record-foot';
  if((item.manual||[]).some(Boolean)){
    // Windows marks records that contain a manual candidate choice and gives
    // them three samples per epoch; show the same tag here.
    const revised=document.createElement('span');revised.className='revised';revised.textContent='曾經手動選字';
    foot.append(revised);
  }
  const readings=document.createElement('span');readings.className='readings';
  const readingLabel=document.createElement('span');readingLabel.textContent='逐字注音';
  const readingValue=document.createElement('strong');readingValue.textContent=readingSequence(item);
  readings.append(readingLabel,readingValue);foot.append(readings);
  card.append(head,composed(item),foot);
  return card;
}
function runCard(item){
  const card=document.createElement('article');card.className='run';
  const top=document.createElement('div');top.className='run-top';
  const time=document.createElement('strong');time.textContent=item.completed_at;
  const tags=document.createElement('div');tags.className='tagrow';
  for(const text of ['rank '+item.rank,'alpha '+item.alpha,'dropout '+item.dropout,item.target_modules,
      '本次 '+item.record_count+' 筆','累計 '+item.cumulative_count+' 筆','步數 '+item.optimizer_steps]){
    const tag=document.createElement('span');tag.className='tag';tag.textContent=text;tags.append(tag);
  }
  if(item.model_path===activeModelPath){
    // Mirrors the Windows manager: a completed model that is already the
    // configured one shows the loaded state instead of another reload button.
    const loaded=document.createElement('span');loaded.className='tag';loaded.textContent='使用中・載入成功';
    top.append(time,tags,loaded);
  }else{
    const button=document.createElement('button');button.className='ghost tiny';
    button.textContent='使用此模型 →';
    button.onclick=async()=>{
      try{await api('use-model',{id:item.id});await refresh();}
      catch(error){
        let status=top.querySelector('.run-error');
        if(!status){status=document.createElement('span');status.className='tag error run-error';top.insertBefore(status,button);}
        status.textContent=error.message;
      }
    };
    top.append(time,tags,button);
  }
  card.append(top);
  const path=document.createElement('div');path.className='path';path.textContent=item.model_path;card.append(path);
  return card;
}
async function refresh() {
  try {
    const state=await api('state');
    activeModelPath=state.active_model_path||'';
    const job=state.job;
    message.className='notice'+(job.state==='failed'?' error':'');
    message.textContent=job.state==='running' ? ({fetch:'正在下載模型',check:'正在檢查模型更新',install:'正在安裝 Trainer',train:'正在訓練及匯出模型'}[job.kind])+(job.progress?'・'+job.progress:'')
      : job.state==='idle'?'目前沒有工作':job.kind==='train'&&job.state==='completed'?'個人化模型已完成':job.kind+'：'+({completed:'完成',failed:'失敗',cancelled:'已取消'}[job.state]||job.state);
    document.getElementById('log').textContent=job.log || '';
    document.getElementById('log').style.display=job.log?'block':'none';
    const progress=document.getElementById('progress');progress.style.display=job.state==='running'?'block':'none';
    if(job.percent!==null){progress.value=job.percent;}else{progress.removeAttribute('value');}
    document.getElementById('check').disabled=job.state==='running';
    document.getElementById('fetch').disabled=job.state==='running';
    document.getElementById('install-trainer').disabled=job.state==='running';
    document.getElementById('train').disabled=job.state==='running' || !state.model_ready || !state.trainer_ready;
    document.getElementById('cancel').disabled=job.state!=='running';
    const modelStatus=document.getElementById('model-status');modelStatus.replaceChildren();
    const modelChip=document.createElement('span');modelChip.className='chip '+(state.model_ready?'trained':'');
    modelChip.textContent=state.model_ready?'已就緒':'尚未下載';modelStatus.append(modelChip);
    if(state.model_ready){const revision=document.createElement('span');revision.className='revision';revision.textContent=state.revision;modelStatus.append(revision);}
    if(state.model_update_available===true){const update=document.createElement('span');update.className='tag';update.textContent='有新版本可用';modelStatus.append(update);}
    else if(state.model_update_available===false){const current=document.createElement('span');current.className='tag';current.textContent='已是最新版本';modelStatus.append(current);}
    document.getElementById('trainer-status').textContent=state.trainer_ready ? 'LoRA Trainer 已安裝' :
      '找不到 llavon-lora，請安裝選配的 LoRA Trainer 元件。';
    const selected=document.getElementById('record-state').value;
    // Records typed while the page is open show up on the next poll and join
    // the selection by default, so an open page always mirrors the database.
    if(reviewedIds===null)reviewedIds=[];
    const fresh=await api('pending-ids');
    for(const id of fresh){
      if(!reviewedIds.includes(id))reviewedIds.push(id);
      if(!selectedIds.has(id))selectedIds.set(id,true);
    }
    pendingIds=fresh;
    updateEstimate();
    const listing=await api('records?state='+selected+'&offset='+recordOffset);
    const page=Math.floor(recordOffset/PAGE_SIZE)+1;
    const pages=Math.max(1,Math.ceil((listing.total||0)/PAGE_SIZE));
    document.getElementById('page-summary').textContent='第 '+page+' / '+pages+' 頁・共 '+(listing.total||0)+' 筆';
    document.getElementById('previous').disabled=recordOffset===0;
    document.getElementById('next').disabled=!listing.has_more;
    const records=listing.rows;
    const list=document.getElementById('records');list.replaceChildren();
    if (!records.length) list.innerHTML='<p class="empty">目前沒有這類紀錄。</p>';
    else for (const item of records) list.append(recordCard(item,selected));
    const runs=await api('runs'), view=document.getElementById('runs');view.replaceChildren();
    if (!runs.length) view.innerHTML='<p class="empty">尚無已完成模型。</p>';
    else for(const item of runs) view.append(runCard(item));
  } catch(error){message.className='notice error';message.textContent=error.message;}
}
async function act(path, body){try{await api(path,body);await refresh();}catch(error){message.className='notice error';message.textContent=error.message;}}
document.getElementById('fetch').onclick=()=>act('fetch',{});
document.getElementById('install-trainer').onclick=()=>act('install-trainer',{});
document.getElementById('check').onclick=()=>act('check',{});
document.getElementById('train').onclick=async()=>{
  try{
    const ids=reviewedIds.filter(id=>selectedIds.get(id)!==false);
    if(!ids.length)throw Error('請至少選取一筆訓練紀錄');
    const options=Object.fromEntries(fields.map(([name])=>[name,document.getElementById(name).value]));
    options.device=document.getElementById('device').value;
    options.dtype=document.getElementById('dtype').value;
    options.shuffle=document.getElementById('shuffle').checked?'1':'0';
    if(!confirm(`以 ${ids.length} 筆資料開始訓練？未勾選的紀錄將被排除。`))return;
    await act('train',{ids,reviewed:reviewedIds,options});
  }catch(error){message.className='notice error';message.textContent=error.message;}
};
document.getElementById('cancel').onclick=()=>act('cancel',{});
document.getElementById('record-state').onchange=()=>{recordOffset=0;refresh();};
document.getElementById('reload-records').onclick=()=>{recordOffset=0;refresh();};
document.getElementById('select-all').onclick=()=>{for(const id of reviewedIds||[])selectedIds.set(id,true);refresh();};
document.getElementById('clear-all').onclick=()=>{for(const id of reviewedIds||[])selectedIds.set(id,false);refresh();};
document.getElementById('previous').onclick=()=>{recordOffset=Math.max(0,recordOffset-PAGE_SIZE);refresh();};
document.getElementById('next').onclick=()=>{recordOffset+=PAGE_SIZE;refresh();};
if (!token) {message.textContent='請從輸入法選單重新開啟管理頁面。';}
else {
  // The character table decides the readings shown for each character.
  api('readings').then(table=>{readingsTable=table||{};}).catch(()=>{}).then(()=>{refresh();setInterval(refresh,3000);});
}
</script></html>)LLAVON";

constexpr std::string_view logo_data_uri =
    "data:image/png;base64,"
    "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";


std::string render_page() {
    std::string html(page);
    constexpr std::string_view placeholder = "@@LOGO@@";
    for (auto position = html.find(placeholder); position != std::string::npos;
         position = html.find(placeholder, position + logo_data_uri.size())) {
        html.replace(position, placeholder.size(), logo_data_uri);
    }
    return html;
}

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string random_token() {
    std::random_device random;
    constexpr char hex[] = "0123456789abcdef";
    std::string token;
    for (int i = 0; i < 32; ++i) { const auto byte = static_cast<unsigned char>(random()); token += hex[byte >> 4]; token += hex[byte & 15]; }
    return token;
}

void send_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const auto count = ::send(fd, data.data(), data.size(), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return;
        data.remove_prefix(static_cast<std::size_t>(count));
    }
}

// Two builds of the manager must not serve the same session: a stale process
// would keep showing an outdated interface after an update. The lock file
// records the owning PID and this stamp, and a newer build takes over an idle
// instance (a running job is left alone).
constexpr std::string_view build_stamp = __DATE__ " " __TIME__;

std::string http_get_state(const std::string& url) {
    const auto scheme_end = url.find("://");
    const auto host_start = scheme_end == std::string::npos ? std::string::npos : scheme_end + 3;
    if (host_start == std::string::npos) return {};
    const auto slash = url.find('/', host_start);
    const auto authority = url.substr(host_start, slash == std::string::npos ? std::string::npos : slash - host_start);
    const auto colon = authority.rfind(':');
    if (colon == std::string::npos) return {};
    int port = 0;
    const auto digits = authority.substr(colon + 1);
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), port);
    if (parsed.ec != std::errc() || parsed.ptr != digits.data() + digits.size() || port <= 0 || port > 65535) return {};
    const auto hash = url.find('#');
    const std::string token = hash == std::string::npos ? std::string{} : url.substr(hash + 1);

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return {};
    timeval timeout{1, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) { ::close(fd); return {}; }
    send_all(fd, "GET /api/state HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                 "\r\nX-Llavon-Token: " + token + "\r\nConnection: close\r\n\r\n");
    std::string response;
    std::array<char, 4096> buffer{};
    while (response.size() < 65536) {
        const auto count = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (count <= 0) break;
        response.append(buffer.data(), static_cast<std::size_t>(count));
    }
    ::close(fd);
    const auto body = response.find("\r\n\r\n");
    return body == std::string::npos ? std::string{} : response.substr(body + 4);
}

struct Request {
    std::string method, path, origin, host, token, content_type, body;
};

Request read_request(int fd) {
    Request request;
    std::string raw;
    std::array<char, 4096> buffer{};
    while (raw.find("\r\n\r\n") == std::string::npos) {
        const auto count = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (count <= 0) throw std::runtime_error("incomplete HTTP headers");
        raw.append(buffer.data(), static_cast<std::size_t>(count));
        if (raw.size() > 65536) throw std::runtime_error("HTTP request is too large");
    }
    const auto header_end = raw.find("\r\n\r\n");
    const auto first_line = raw.find("\r\n");
    const auto first_space = raw.find(' ');
    const auto second_space = raw.find(' ', first_space + 1);
    if (first_space == std::string::npos || second_space == std::string::npos || second_space > first_line ||
        raw.substr(second_space + 1, first_line - second_space - 1) != "HTTP/1.1")
        throw std::runtime_error("invalid HTTP request");
    request.method = raw.substr(0, first_space);
    request.path = raw.substr(first_space + 1, second_space - first_space - 1);
    std::size_t length = 0;
    for (auto pos = first_line + 2; pos < header_end;) {
        const auto end = raw.find("\r\n", pos);
        if (end == std::string::npos || end > header_end) throw std::runtime_error("invalid HTTP header");
        const auto line = raw.substr(pos, end - pos);
        const auto colon = line.find(':');
        if (colon == std::string::npos) throw std::runtime_error("invalid HTTP header");
        std::string key = line.substr(0, colon);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch){ return static_cast<char>(std::tolower(ch)); });
        const auto value = trim(line.substr(colon + 1));
        if (key == "host") request.host = value;
        else if (key == "origin") request.origin = value;
        else if (key == "x-llavon-token") request.token = value;
        else if (key == "content-type") request.content_type = value;
        else if (key == "content-length") {
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), length);
            if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() || length > 16384)
                throw std::runtime_error("invalid Content-Length");
        } else if (key == "transfer-encoding") throw std::runtime_error("chunked requests are not supported");
        pos = end + 2;
    }
    request.body = raw.substr(header_end + 4);
    if (request.body.size() > length) throw std::runtime_error("unexpected HTTP body");
    while (request.body.size() < length) {
        const auto count = ::recv(fd, buffer.data(), std::min(buffer.size(), length - request.body.size()), 0);
        if (count <= 0) throw std::runtime_error("incomplete HTTP body");
        request.body.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return request;
}

void respond(int fd, int status, std::string_view type, std::string_view body) {
    const std::string header = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Error") +
        "\r\nContent-Type: " + std::string(type) + "; charset=utf-8\r\nCache-Control: no-store\r\n" +
        "Referrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\nContent-Security-Policy: default-src 'none'; "
        "script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; img-src 'self' data:\r\n" +
        "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    send_all(fd, header); send_all(fd, body);
}

fs::path executable_path(const char* argv0) {
#ifdef __APPLE__
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) return fs::weakly_canonical(buffer.c_str());
#else
    std::error_code error;
    const auto path = fs::read_symlink("/proc/self/exe", error);
    if (!error) return path;
#endif
    return fs::absolute(argv0);
}

fs::path default_state() {
    if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state)
        return fs::path(state) / "llavon-ime" / "training";
    const char* home = std::getenv("HOME");
    if (!home || !*home) throw std::runtime_error("HOME is not set");
#ifdef __APPLE__
    return fs::path(home) / "Library" / "Application Support" / "llavon-ime" / "training";
#else
    return fs::path(home) / ".local" / "state" / "llavon-ime" / "training";
#endif
}

void open_browser(const std::string& url, bool enabled) {
    if (!enabled) return;
    const pid_t child = ::fork();
    if (child == 0) {
        // xdg-open may wait for the browser. Do not block the HTTP server or
        // leave an unreaped browser child in the manager process.
        const pid_t launcher = ::fork();
        if (launcher < 0) _exit(127);
        if (launcher > 0) _exit(0);
        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) { ::dup2(devnull, STDOUT_FILENO); ::dup2(devnull, STDERR_FILENO); ::close(devnull); }
#ifdef __APPLE__
        ::execl("/usr/bin/open", "open", url.c_str(), static_cast<char*>(nullptr));
#else
        ::execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char*>(nullptr));
#endif
        _exit(127);
    }
    if (child > 0) {
        int status;
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            std::cerr << "Could not open a browser; open " << url << '\n';
    }
}

std::string last_log(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return {};
    const auto size = stream.tellg();
    if (size > 12000) stream.seekg(-12000, std::ios::end);
    else stream.seekg(0);
    std::string data((std::istreambuf_iterator<char>(stream)), {});
    if (size > 12000) { const auto line = data.find('\n'); if (line != std::string::npos) data.erase(0, line + 1); }
    return data;
}

fs::path config_file() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) throw std::runtime_error("HOME is not set");
#ifdef __APPLE__
    const fs::path root = std::getenv("XDG_CONFIG_HOME") ? fs::path(std::getenv("XDG_CONFIG_HOME")) : fs::path(home) / ".config";
    return root / "llavon-ime" / "config.json";
#else
    if (const char* value = std::getenv("LLAVON_IME_CONFIG_PATH"); value && *value) return value;
    const fs::path root = std::getenv("XDG_CONFIG_HOME") ? fs::path(std::getenv("XDG_CONFIG_HOME")) : fs::path(home) / ".config";
    return root / "fcitx5" / "conf" / "llavon-ime.conf";
#endif
}

// The model the input method currently points at; the history uses it to mark
// the run that is already loaded (Windows shows 載入成功 the same way).
std::string configured_model_path() {
    try {
        const auto config = config_file();
        std::ifstream input(config);
        if (!input) return {};
#ifdef __APPLE__
        return json::parse(input).value("model_path", std::string{});
#else
        std::string line;
        while (std::getline(input, line)) {
            if (!line.starts_with("ModelPath=")) continue;
            auto value = trim(line.substr(std::string("ModelPath=").size()));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
            std::string unescaped;
            for (std::size_t index = 0; index < value.size(); ++index) {
                if (value[index] == '\\' && index + 1 < value.size()) ++index;
                unescaped += value[index];
            }
            return unescaped;
        }
        return {};
#endif
    } catch (...) { return {}; }
}

void use_model(const fs::path& path) {
    const auto config = config_file();
    fs::create_directories(config.parent_path());
    const auto temporary = fs::path(config.string() + ".lora.partial");
#ifdef __APPLE__
    json values = json::object();
    if (fs::is_regular_file(config)) values = json::parse(std::ifstream(config));
    if (!values.is_object()) throw std::runtime_error("invalid input method settings");
    values["model_path"] = path.string();
    { std::ofstream output(temporary, std::ios::trunc); output << values.dump(2) << '\n';
      if (!output) throw std::runtime_error("cannot save input method settings"); }
#else
    std::vector<std::string> lines;
    std::ifstream input(config);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.starts_with("ModelPath=")) lines.push_back(line);
    }
    // Fcitx INI accepts quoted paths with spaces and backslashes.
    std::string escaped;
    for (char ch : path.string()) {
        if (ch == '\\' || ch == '"') escaped += '\\';
        escaped += ch;
    }
    { std::ofstream output(temporary, std::ios::trunc); for (const auto& value : lines) output << value << '\n';
      output << "ModelPath=\"" << escaped << "\"\n";
      if (!output) throw std::runtime_error("cannot save input method settings"); }
#endif
    if (::chmod(temporary.c_str(), 0600) != 0) throw std::runtime_error("cannot protect input method settings");
    fs::rename(temporary, config);
#ifdef __APPLE__
    (void)::notify_post("org.llavon-ime.lora.model-changed");
#else
    const auto child = ::fork();
    if (child == 0) { ::execlp("fcitx5-remote", "fcitx5-remote", "-r", static_cast<char*>(nullptr)); _exit(127); }
    if (child > 0) { int status; while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {} }
#endif
}

bool has_pinned_trainer_stamp(const fs::path& executable) {
    try {
        return nlohmann::json::parse(std::ifstream(executable.parent_path() / "trainer-release.json")).at("commit")
               == LLAVON_IME_LORA_PINNED_COMMIT;
    } catch (...) { return false; }
}

// TorchSharp needs the native libraries next to the executable; a stale
// installation that only carries the executable must not look ready.
bool has_trainer_libraries(const fs::path& directory, int depth = 0) {
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(directory, error)) {
        const auto name = entry.path().filename().string();
        if (name.ends_with(".so") || name.find(".so.") != std::string::npos || name.ends_with(".dylib")) return true;
        if (depth < 3 && entry.is_directory() && has_trainer_libraries(entry.path(), depth + 1)) return true;
    }
    return false;
}

bool trainer_usable(const fs::path& executable) {
    return fs::is_regular_file(executable) && has_pinned_trainer_stamp(executable) &&
           has_trainer_libraries(executable.parent_path());
}

fs::path trainer_path(const fs::path& state) {
    if (const char* override = std::getenv("LLAVON_IME_LORA_CLI_PATH"); override && *override)
        return fs::absolute(override);
    const auto managed = state / "tools" / "lora" / "llavon-lora";
    const auto system = fs::path(LLAVON_IME_INSTALLED_LORA_TRAINER_PATH);
    if (trainer_usable(managed)) return managed;
    if (trainer_usable(system)) return system;
    return fs::is_regular_file(managed) ? managed : system;
}

bool trainer_ready(const fs::path& executable, const fs::path& state) {
    if (!fs::is_regular_file(executable) || ::access(executable.c_str(), X_OK) != 0) return false;
    const auto managed = state / "tools" / "lora" / "llavon-lora";
    const auto system = fs::path(LLAVON_IME_INSTALLED_LORA_TRAINER_PATH);
    if (executable != managed && executable != system) return true;  // explicit development override
    return trainer_usable(executable);
}

json query_database(const fs::path& path, const char* sql, int columns) {
    if (!fs::is_regular_file(path)) return json::array();
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        const std::string error = db ? sqlite3_errmsg(db) : "cannot open training database";
        sqlite3_close(db); throw std::runtime_error(error);
    }
    sqlite3_busy_timeout(db, 1000);
    sqlite3_stmt* stmt = nullptr;
    json result = json::array();
    try {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
        int rc;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            json row = json::array();
            for (int i = 0; i < columns; ++i) {
                const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
                row.push_back(text ? text : "");
            }
            result.push_back(std::move(row));
        }
        if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
    } catch (...) { sqlite3_finalize(stmt); sqlite3_close(db); throw; }
    sqlite3_finalize(stmt); sqlite3_close(db);
    return result;
}

struct Options {
    fs::path state, db, cli, tables, trainer;
    bool browser = true;
    int idle_seconds = 120;
};

// Windows exposes the same override for its dev/test asset root.
fs::path assets_root(const Options& options) {
    if (const char* override = std::getenv("LLAVON_IME_LORA_ASSETS_DIR"); override && *override)
        return fs::absolute(override);
    return options.state / "assets";
}

fs::path runs_root(const Options& options) {
    if (const char* override = std::getenv("LLAVON_IME_LORA_ASSETS_DIR"); override && *override)
        return fs::absolute(override) / "runs";
    return options.state / "runs";
}

Options parse_options(int argc, char** argv) {
    Options options;
    options.state = default_state();
    if (const char* db = std::getenv("LLAVON_IME_TRAINING_DATABASE_PATH"); db && *db) options.db = db;
    const auto binary = executable_path(argv[0]);
    options.cli = binary.parent_path() / "llavon-ime-lora";
    options.tables = binary.parent_path().parent_path() / "share" / "llavon-ime" / "tables";
    if (!fs::exists(options.tables)) options.tables = LLAVON_IME_GUI_INSTALLED_TABLES_DIR;
    for (int i = 1; i < argc; ++i) {
        const std::string name = argv[i];
        if (name == "--no-browser") { options.browser = false; continue; }
        if (i + 1 >= argc) throw std::invalid_argument("missing value for " + name);
        const std::string value = argv[++i];
        if (name == "--state-dir") options.state = value;
        else if (name == "--db") options.db = value;
        else if (name == "--cli") options.cli = value;
        else if (name == "--tables-dir") options.tables = value;
        else if (name == "--idle-seconds") options.idle_seconds = std::stoi(value);
        else throw std::invalid_argument("unknown option: " + name);
    }
    if (options.idle_seconds < 5) throw std::invalid_argument("idle timeout must be at least 5 seconds");
    options.state = fs::absolute(options.state);
    options.trainer = trainer_path(options.state);
    if (!options.db.empty()) options.db = fs::absolute(options.db);
    options.cli = fs::absolute(options.cli);
    options.tables = fs::absolute(options.tables);
    options.trainer = fs::absolute(options.trainer);
    return options;
}

struct Job {
    std::string kind, state = "idle";
    pid_t pid = -1;
    fs::path log, output;
    bool cancelling = false;
};

class Gui {
public:
    explicit Gui(Options options) : options_(std::move(options)),
        db_(options_.db.empty() ? options_.state / "commits.sqlite3" : options_.db) {}

    int run() {
        fs::create_directories(options_.state);
        struct stat directory {};
        if (::lstat(options_.state.c_str(), &directory) || !S_ISDIR(directory.st_mode) || directory.st_uid != ::getuid() ||
            ::chmod(options_.state.c_str(), 0700)) throw std::runtime_error("unsafe training data directory");
        const auto lock_path = options_.state / "gui.lock";
        lock_ = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
        struct stat file {};
        if (lock_ < 0 || ::fstat(lock_, &file) || !S_ISREG(file.st_mode) || file.st_uid != ::getuid() || ::fchmod(lock_, 0600))
            throw std::runtime_error("unsafe GUI lock file");
        // Browser launchers and the CLI must not retain the singleton lock or
        // listening socket across exec (especially when a job outlives a tab).
        if (::fcntl(lock_, F_SETFD, FD_CLOEXEC)) throw std::runtime_error("cannot protect GUI lock descriptor");
        if (::flock(lock_, LOCK_EX | LOCK_NB)) {
            if (errno != EWOULDBLOCK) throw std::runtime_error("cannot lock GUI session");
            std::string url, stamp;
            pid_t owner = 0;
            {
                std::array<char, 512> info{};
                const auto size = ::pread(lock_, info.data(), info.size(), 0);
                if (size > 0) {
                    std::istringstream stream(std::string(info.data(), static_cast<std::size_t>(size)));
                    std::string pid_line, stamp_line;
                    std::getline(stream, url);
                    std::getline(stream, pid_line);
                    std::getline(stream, stamp_line);
                    url = trim(url); stamp = trim(stamp_line);
                    const auto pid_text = trim(pid_line);
                    if (!pid_text.empty()) {
                        try { owner = static_cast<pid_t>(std::stol(pid_text)); } catch (...) { owner = 0; }
                    }
                }
            }
            bool acquired = false;
            if (owner > 0 && !stamp.empty() && stamp != std::string(build_stamp)) {
                bool busy = false;
                try {
                    const auto state = json::parse(http_get_state(url));
                    busy = state.value("job", json::object()).value("state", std::string{}) == "running";
                } catch (...) { busy = false; }
                if (!busy) {
                    (void)::kill(owner, SIGTERM);
                    for (int attempt = 0; attempt < 50; ++attempt) {
                        ::usleep(100000);
                        if (::flock(lock_, LOCK_EX | LOCK_NB) == 0) { acquired = true; break; }
                    }
                }
            }
            if (!acquired) {
                if (!url.starts_with("http://127.0.0.1:")) throw std::runtime_error("existing GUI is starting; retry");
                if (!options_.browser) std::cout << "URL=" << url << std::endl;
                open_browser(url, options_.browser);
                return 0;
            }
        }
        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_ < 0) throw std::runtime_error("cannot create GUI socket");
        if (::fcntl(listen_, F_SETFD, FD_CLOEXEC)) throw std::runtime_error("cannot protect GUI socket descriptor");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || ::listen(listen_, 8))
            throw std::runtime_error("cannot listen on loopback");
        socklen_t length = sizeof(address);
        if (::getsockname(listen_, reinterpret_cast<sockaddr*>(&address), &length))
            throw std::runtime_error("cannot find GUI port");
        port_ = ntohs(address.sin_port);
        token_ = random_token();
        const std::string url = "http://127.0.0.1:" + std::to_string(port_) + "/#" + token_;
        const std::string lock_contents = url + "\n" + std::to_string(::getpid()) + "\n" + std::string(build_stamp) + "\n";
        if (::ftruncate(lock_, 0) ||
            ::pwrite(lock_, lock_contents.data(), lock_contents.size(), 0) != static_cast<ssize_t>(lock_contents.size()))
            throw std::runtime_error("cannot store GUI address");
        if (!options_.browser) std::cout << "URL=" << url << std::endl;
        open_browser(url, options_.browser);
        last_seen_ = Clock::now();
        while (!stop_requested) {
            update_job();
            if (job_.pid < 0 && Clock::now() - last_seen_ > std::chrono::seconds(options_.idle_seconds)) break;
            pollfd descriptor{listen_, POLLIN, 0};
            const auto ready = ::poll(&descriptor, 1, 500);
            if (ready < 0 && errno != EINTR) throw std::runtime_error("GUI socket poll failed");
            if (ready <= 0 || !(descriptor.revents & POLLIN)) continue;
            const int client = ::accept(listen_, nullptr, nullptr);
            if (client < 0) continue;
            if (::fcntl(client, F_SETFD, FD_CLOEXEC)) { ::close(client); continue; }
            timeval timeout{2, 0};
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            try { handle(client); }
            catch (const std::exception& error) { respond(client, 400, "application/json", json{{"error", error.what()}}.dump()); }
            ::close(client);
        }
        if (job_.pid >= 0) {
            (void)::kill(-job_.pid, SIGTERM);
            for (int i = 0; i < 20; ++i) {
                update_job();
                if (job_.pid < 0) break;
                ::usleep(100000);
            }
            if (job_.pid >= 0) { (void)::kill(-job_.pid, SIGKILL); (void)::waitpid(job_.pid, nullptr, 0); }
        }
        return 0;
    }

private:
    void update_job() {
        if (job_.pid < 0) return;
        int status;
        const auto ended = ::waitpid(job_.pid, &status, WNOHANG);
        if (ended == 0) return;
        if (ended < 0 && errno == EINTR) return;
        job_.state = job_.cancelling ? "cancelled" :
                     (ended > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "completed" : "failed");
        if (job_.kind == "install" && job_.state == "completed" && !std::getenv("LLAVON_IME_LORA_CLI_PATH"))
            options_.trainer = options_.state / "tools" / "lora" / "llavon-lora";
        job_.pid = -1;
    }

    void start_job(const std::string& kind, std::vector<std::string> args, fs::path output) {
        update_job();
        if (job_.pid >= 0) throw std::runtime_error("已有工作進行中");
        job_ = Job{.kind = kind, .state = "running", .log = options_.state / "gui-job.log", .output = std::move(output)};
        std::ofstream(job_.log, std::ios::trunc).close();
        const pid_t child = ::fork();
        if (child < 0) throw std::runtime_error("cannot start CLI");
        if (child == 0) {
            ::setsid();
            if (kind == "train") ::setenv("LLAVON_IME_LORA_CLI_PATH", options_.trainer.c_str(), 1);
            const int log = ::open(job_.log.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
            if (log < 0 || ::dup2(log, STDOUT_FILENO) < 0 || ::dup2(log, STDERR_FILENO) < 0) _exit(127);
            ::close(log);
            std::vector<std::string> strings{options_.cli.string()};
            strings.insert(strings.end(), args.begin(), args.end());
            std::vector<char*> argv;
            for (auto& item : strings) argv.push_back(item.data());
            argv.push_back(nullptr);
            ::execv(argv[0], argv.data());
            _exit(127);
        }
        job_.pid = child;
    }

    json records(const std::string& requested) const {
        const auto question = requested.find('?');
        const auto params = question == std::string::npos ? "" : requested.substr(question + 1);
        std::string state = "pending";
        int offset = 0;
        for (std::size_t start = 0; start < params.size();) {
            const auto end = params.find('&', start);
            const auto part = params.substr(start, end == std::string::npos ? end : end - start);
            if (part.starts_with("state=")) state = part.substr(6);
            else if (part.starts_with("offset=")) {
                const auto digits = part.substr(7);
                const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), offset);
                if (parsed.ec != std::errc() || parsed.ptr != digits.data() + digits.size() || offset < 0 || offset > 100000)
                    throw std::runtime_error("invalid record offset");
            } else if (!part.empty()) throw std::runtime_error("invalid record filter");
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (state != "pending" && state != "excluded" && state != "trained")
            throw std::runtime_error("invalid record state");
        auto rows = query_database(db_,
            ("WITH recent AS (SELECT id,committed_at,context,answer FROM commits WHERE state='" + state + "' "
            "ORDER BY committed_at DESC,id DESC LIMIT " + std::to_string(kRecordsPerPage + 1) + " OFFSET " +
            std::to_string(offset) + ") "
            "SELECT c.id,c.committed_at,c.context,c.answer,r.reading,r.manually_selected FROM recent c "
            "LEFT JOIN readings r ON r.commit_id=c.id ORDER BY c.committed_at DESC,c.id DESC,r.position").c_str(), 6);
        json entries = json::array();
        for (const auto& row : rows) {
            if (entries.empty() || entries.back().at("id") != row[0])
                entries.push_back({{"id",row[0]}, {"committed_at",row[1]}, {"context",row[2]},
                                   {"answer",row[3]}, {"readings",json::array()}, {"manual",json::array()}});
            if (!row[4].get<std::string>().empty()) {
                entries.back()["readings"].push_back(row[4]);
                entries.back()["manual"].push_back(row[5] == "1");
            }
        }
        const bool has_more = entries.size() > kRecordsPerPage;
        if (has_more) entries.erase(entries.end() - 1);
        const auto counted = query_database(db_,
            ("SELECT COUNT(*) FROM commits WHERE state='" + state + "'").c_str(), 1);
        const int total = counted.empty() ? 0 : std::stoi(counted[0][0].get<std::string>());
        return {{"rows",entries}, {"has_more",has_more}, {"total",total}};
    }

    json runs() const {
        const auto columns = query_database(db_, "PRAGMA table_info(lora_runs)", 3);
        const bool extended = std::any_of(columns.begin(), columns.end(), [](const json& col){return col[1] == "rank";});
        const auto rows = query_database(db_, extended ?
            "SELECT completed_at,record_count,model_path,optimizer_steps,rank,alpha,dropout,target_modules,id,"
            "SUM(record_count) OVER (ORDER BY id) FROM lora_runs ORDER BY id DESC LIMIT 20" :
            "SELECT completed_at,record_count,model_path,id,SUM(record_count) OVER (ORDER BY id) "
            "FROM lora_runs ORDER BY id DESC LIMIT 20", extended ? 10 : 5);
        json entries = json::array();
        for (const auto& row : rows) {
            entries.push_back({{"completed_at",row[0]}, {"record_count",std::stoi(row[1].get<std::string>())},
                               {"model_path",row[2]}, {"cumulative_count",std::stoi(row[extended ? 9 : 4].get<std::string>())},
                               {"optimizer_steps",extended ? row[3] : json("0")},
                               {"rank",extended ? row[4] : json("8")}, {"alpha",extended ? row[5] : json("16")},
                               {"dropout",extended ? row[6] : json("0")},
                               {"target_modules",extended ? row[7] : json("q_proj,v_proj")},
                               {"id",extended ? row[8] : row[3]}});
        }
        return entries;
    }

    json pending_ids() const {
        const auto rows = query_database(db_, "SELECT id FROM commits WHERE state='pending' ORDER BY id", 1);
        json ids = json::array();
        for (const auto& row : rows) ids.push_back(row[0]);
        return ids;
    }

    // The IME candidate table is reading -> characters; invert it once so the
    // page can look up every reading a character may have.
    json readings_table() {
        if (!readings_loaded_) {
            readings_loaded_ = true;
            try {
                const auto table = json::parse(std::ifstream(options_.tables / "bopomofo_char.json"));
                json inverse = json::object();
                for (auto entry = table.begin(); entry != table.end(); ++entry) {
                    if (!entry.value().is_array()) continue;
                    for (const auto& candidate : entry.value()) {
                        if (!candidate.is_string()) continue;
                        auto& readings = inverse[candidate.get<std::string>()];
                        if (!readings.is_array()) readings = json::array();
                        if (std::find(readings.begin(), readings.end(), entry.key()) == readings.end())
                            readings.push_back(entry.key());
                    }
                }
                readings_cache_ = std::move(inverse);
            } catch (...) { readings_cache_ = json::object(); }
        }
        return readings_cache_;
    }

    json state() {
        update_job();
        const auto data = last_log(job_.log);
        std::string progress;
        json percent = nullptr;
        if (job_.kind == "train") {
            const auto step = data.rfind("step=");
            if (step != std::string::npos) {
                progress = trim(data.substr(step, std::min<std::size_t>(50, data.size() - step)));
                int current = 0, total = 0;
                if (std::sscanf(data.c_str() + step, "step=%d/%d", &current, &total) == 2 && total > 0)
                    percent = 5 + 80 * std::clamp(static_cast<double>(current) / total, 0.0, 1.0);
            }
        } else if (job_.kind == "fetch" && job_.pid >= 0) {
            const auto assets = assets_root(options_);
            if (fs::exists(assets)) {
                for (const auto& entry : fs::directory_iterator(assets)) {
                    if (!entry.is_directory()) continue;
                    const auto part = entry.path() / "model.safetensors.partial";
                    if (fs::is_regular_file(part)) progress = std::to_string(fs::file_size(part) / 1048576) + " MiB 已下載";
                }
            }
        } else if (job_.kind == "install" && job_.pid >= 0) {
            const auto partial = options_.state / "tools" / "lora" / "trainer.tar.gz.partial";
            if (fs::is_regular_file(partial)) progress = std::to_string(fs::file_size(partial) / 1048576) + " MiB 已下載";
        }
        const auto assets = assets_root(options_);
        const auto revision = trim(last_log(assets / "current.revision"));
        const bool ready = revision.size() == 40 && fs::is_regular_file(assets / revision / "config.json") &&
            fs::is_regular_file(assets / revision / "ime_vocab.json") &&
            fs::is_regular_file(assets / revision / "model.safetensors");
        json update = nullptr;
        if (job_.kind == "check" && job_.state == "completed") {
            if (data.find("update-available=true") != std::string::npos) update = true;
            else if (data.find("update-available=false") != std::string::npos) update = false;
        }
        return {{"job", {{"kind",job_.kind}, {"state",job_.state}, {"progress",progress}, {"percent",percent}, {"log",data}}},
                {"model_ready", ready}, {"revision", ready ? revision : ""},
                {"model_update_available", update},
                {"active_model_path", configured_model_path()},
                {"trainer_ready", trainer_ready(options_.trainer, options_.state)}};
    }

    void command(const std::string& action, const std::string& id) {
        if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::invalid_argument("invalid record ID");
        const auto child = ::fork();
        if (child < 0) throw std::runtime_error("cannot start CLI");
        if (child == 0) {
            ::execl(options_.cli.c_str(), options_.cli.c_str(), action.c_str(), "--db", db_.c_str(), "--id", id.c_str(),
                    static_cast<char*>(nullptr));
            _exit(127);
        }
        int status;
        while (::waitpid(child, &status, 0) < 0) { if (errno != EINTR) throw std::runtime_error("cannot wait for CLI"); }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) throw std::runtime_error("紀錄操作失敗");
    }

    void handle(int fd) {
        const auto request = read_request(fd);
        const std::string host = "127.0.0.1:" + std::to_string(port_);
        if (request.host != host || (!request.origin.empty() && request.origin != "http://" + host)) {
            respond(fd, 403, "application/json", R"({"error":"forbidden origin"})"); return;
        }
        if (request.method == "GET" && request.path == "/") {
            respond(fd, 200, "text/html", render_page()); return;
        }
        if (request.token != token_) {
            respond(fd, 403, "application/json", R"({"error":"unauthorized"})"); return;
        }
        last_seen_ = Clock::now();
        if (request.method == "GET") {
            const json result = request.path == "/api/state" ? state() :
                                request.path == "/api/pending-ids" ? pending_ids() :
                                request.path == "/api/readings" ? readings_table() :
                                request.path.starts_with("/api/records?") || request.path == "/api/records" ? records(request.path) :
                                request.path == "/api/runs" ? runs() : json{{"error","not found"}};
            respond(fd, request.path.starts_with("/api/") && request.path != "/api/state" &&
                        request.path != "/api/pending-ids" && request.path != "/api/readings" &&
                        request.path != "/api/records" && !request.path.starts_with("/api/records?") &&
                        request.path != "/api/runs" ? 404 : 200,
                    "application/json", result.dump()); return;
        }
        if (request.method != "POST" || request.content_type != "application/json")
            throw std::runtime_error("unsupported request");
        const auto body = json::parse(request.body);
        if (!body.is_object()) throw std::runtime_error("expected JSON object");
        if (request.path == "/api/fetch") {
            start_job("fetch", {"fetch-model", "--output-dir", assets_root(options_).string()}, {});
        } else if (request.path == "/api/check") {
            start_job("check", {"check-model", "--output-dir", assets_root(options_).string()}, {});
        } else if (request.path == "/api/install-trainer") {
            start_job("install", {"install-trainer", "--output-dir", (options_.state / "tools" / "lora").string()}, {});
        } else if (request.path == "/api/train") {
            if (!trainer_ready(options_.trainer, options_.state))
                throw std::runtime_error("LoRA Trainer 尚未安裝或版本不符，請安裝／更新 LoRA Trainer");
            const auto ids = body.at("ids");
            const auto reviewed = body.at("reviewed");
            if (!ids.is_array() || ids.empty() || ids.size() > 100000) throw std::runtime_error("請至少選取一筆訓練紀錄");
            if (!reviewed.is_array() || reviewed.size() > 100000) throw std::runtime_error("紀錄清單過大");
            for (const auto& group : {ids, reviewed}) {
                for (const auto& value : group) {
                    if (!value.is_string() || value.get<std::string>().size() != 32 ||
                        value.get<std::string>().find_first_not_of("0123456789abcdef") != std::string::npos)
                        throw std::runtime_error("無效的紀錄 ID");
                }
            }
            const auto params = body.at("options");
            if (!params.is_object()) throw std::runtime_error("無效的訓練參數");
            const auto assets = assets_root(options_);
            std::ifstream revision_file(assets / "current.revision");
            std::string revision;
            revision_file >> revision;
            if (revision.size() != 40 || revision.find_first_not_of("0123456789abcdef") != std::string::npos ||
                !fs::is_regular_file(assets / revision / "model.safetensors"))
                throw std::runtime_error("請先下載基礎模型");
            if (!fs::is_directory(options_.tables)) throw std::runtime_error("找不到輸入法字表");
            const auto output = runs_root(options_) /
                (std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()));
            const auto selected_file = options_.state / "gui-selected-ids.json";
            { std::ofstream selected(selected_file, std::ios::trunc);
              selected << json{{"selected", ids}, {"reviewed", reviewed}}.dump();
              if (!selected) throw std::runtime_error("無法儲存選取的紀錄"); }
            std::vector<std::string> args{"train", "--db", db_.string(), "--model-dir", (assets / revision).string(),
                "--tables-dir", options_.tables.string(), "--output-dir", output.string(),
                "--revision", revision, "--selected-ids", selected_file.string()};
            for (const auto key : {"rank", "alpha", "dropout", "batch-size", "gradient-accumulation", "epochs",
                                   "max-steps", "learning-rate", "weight-decay", "warmup-steps", "max-grad-norm",
                                   "save-every", "seed", "max-seq-length", "target-modules", "device", "dtype", "shuffle"}) {
                args.emplace_back(std::string("--") + key);
                const auto value = params.at(key).get<std::string>();
                if (value.size() > 100) throw std::runtime_error("訓練參數過長");
                args.push_back(value);
            }
            start_job("train", std::move(args), output);
        } else if (request.path == "/api/cancel") {
            update_job();
            if (job_.pid < 0) throw std::runtime_error("沒有執行中的工作");
            job_.cancelling = true;
            if (::kill(-job_.pid, SIGTERM) != 0 && errno != ESRCH) throw std::runtime_error("無法取消工作");
        } else if (request.path == "/api/use-model") {
            update_job();
            if (job_.pid >= 0) throw std::runtime_error("請等待目前工作完成");
            const auto id = body.at("id").get<std::string>();
            if (id.empty() || id.size() > 18 || id.find_first_not_of("0123456789") != std::string::npos)
                throw std::runtime_error("invalid training run");
            const auto rows = query_database(db_, ("SELECT model_path FROM lora_runs WHERE id=" + id).c_str(), 1);
            if (rows.size() != 1) throw std::runtime_error("找不到已完成模型");
            const fs::path model = rows[0][0].get<std::string>();
            if (!fs::is_regular_file(model) || fs::file_size(model) == 0) throw std::runtime_error("模型檔案已不存在");
            use_model(model);
        } else if (request.path.starts_with("/api/records/")) {
            const auto end = request.path.rfind('/');
            const auto action = request.path.substr(end + 1);
            if (action != "exclude" && action != "delete") throw std::runtime_error("unknown record action");
            command(action, request.path.substr(std::string("/api/records/").size(),
                                                end - std::string("/api/records/").size()));
        } else {
            respond(fd, 404, "application/json", R"({"error":"not found"})"); return;
        }
        respond(fd, 200, "application/json", R"({"ok":true})");
    }

    Options options_;
    fs::path db_;
    int lock_ = -1, listen_ = -1, port_ = 0;
    std::string token_;
    Job job_;
    Clock::time_point last_seen_{};
    json readings_cache_ = json::object();
    bool readings_loaded_ = false;
};

} // namespace

int main(int argc, char** argv) {
    ::umask(0077);
    ::signal(SIGPIPE, SIG_IGN);
    ::signal(SIGTERM, request_stop);
    ::signal(SIGINT, request_stop);
    try { return Gui(parse_options(argc, argv)).run(); }
    catch (const std::exception& error) {
        std::cerr << "llavon-ime-lora-gui: " << error.what() << '\n';
        return 1;
    }
}
