import {Spooler,effective,bestIndex} from './engine.mjs';
import {mountGuide} from './guide.mjs';
mountGuide();
const $=id=>document.getElementById(id);
let sim=new Spooler(),running=false,last=0;
const sec=ms=>(ms/1000).toFixed(2);
const time=ms=>`${String(Math.floor(ms/60000)).padStart(2,'0')}:${String(Math.floor(ms/1000)%60).padStart(2,'0')}.${String(Math.floor(ms)%1000).padStart(3,'0')}`;
const printerIcon='<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" aria-hidden="true"><path d="M7 8V3h10v5M7 17H4V9h16v8h-3M7 14h10v7H7zM16 11h1"/></svg>';
function render(){
 const s=sim.stats;
 $('clock').textContent=time(sim.now);
 $('run-status').textContent=sim.done?'จำลองเสร็จสิ้น':running?'กำลังทำงาน':sim.started?'หยุดชั่วคราว':'พร้อมทดลอง';
 $('run-status').classList.toggle('running',running);
 $('play').textContent=running?'Ⅱ หยุดชั่วคราว':sim.done?'✓ เสร็จสิ้น':sim.started?'▶ เล่นต่อ':'▶ เริ่มจำลอง';
 $('play').disabled=sim.done;$('step').disabled=running||sim.done;
 $('printer-count').disabled=sim.started;$('aging').disabled=sim.started;
 $('aging-label').textContent=sim.aging?'เปิด':'ปิด';
 $('stats').innerHTML=[['งานพิมพ์เสร็จ',s.completed,'/ 20 งาน',`เริ่มพิมพ์แล้ว ${s.started} งาน`,'▤'],['เวลารอเฉลี่ย',sec(s.average),'วินาที','นับตั้งแต่เข้าคิวจนเริ่มพิมพ์','◷'],['เวลารอสูงสุด',sec(s.max),'วินาที','จากงานที่เริ่มพิมพ์แล้ว','◴'],['งานที่ได้รับ Aging',s.aged,'งาน',sim.aging?'effective priority น้อยกว่าค่าเดิม':'ปิดการเพิ่มระดับตามเวลารอ','↗']].map(([label,value,unit,foot,icon])=>`<article class="stat"><span class="stat-symbol">${icon}</span><div class="stat-label">${label}</div><div class="stat-value">${value}<small>${unit}</small></div><div class="stat-foot">${foot}</div></article>`).join('');
 $('producers').innerHTML=sim.producers.map(p=>`<div class="producer ${p.blocked?'blocked':''}"><span class="producer-icon">P${p.id}</span><div><strong>Producer ${p.id}</strong><small>${p.done?'จบการทำงาน':p.blocked?'รอช่องว่าง':`ส่งแล้ว ${p.index} / 5`}</small></div></div>`).join('');
 $('queue-count').textContent=`${sim.queue.length} / 10`;
 $('slots').innerHTML=Array.from({length:10},(_,i)=>{const j=sim.queue[i];return `<div class="slot ${j?'filled':''} ${j&&j.id!==-1&&effective(j,sim.now,sim.aging)<j.priority?'aged':''} ${j?.id===-1?'sentinel':''}" title="ช่องอาร์เรย์ ${i}${j?' · '+j.filename:''}">${j?j.id===-1?'STOP':'#'+j.id:String(i+1).padStart(2,'0')}</div>`;}).join('');
 const ordered=[],copy=[...sim.queue];while(copy.length){const i=bestIndex(copy,sim.now,sim.aging);ordered.push(copy.splice(i,1)[0]);}
 $('queue-rows').innerHTML=ordered.map((j,i)=>{const eff=effective(j,sim.now,sim.aging);return `<tr><td><div class="job-name"><span class="job-id">${j.id===-1?'−1':'#'+j.id}</span><div><strong>${j.filename}</strong><small>${j.id===-1?'สัญญาณปิดเครื่อง':'Producer '+j.producer}</small></div></div></td><td>${j.pages}</td><td><span class="priority-pill ${eff<j.priority?'aging-pill':''}">${j.id===-1?'STOP':j.priority}${j.id!==-1&&eff<j.priority?' → '+eff:''}</span></td><td>${sec(sim.now-j.enqueue)} s</td><td><span class="next-tag">${i===0?'ถัดไป':'รอคิว'}</span></td></tr>`;}).join('');
 $('queue-empty').hidden=!!sim.queue.length;
 $('queue-empty').querySelector('strong').textContent=sim.done?'ทุกงานเสร็จแล้ว':sim.started?'ไม่มีงานรอ':'คิวว่าง';
 $('printer-badge').textContent=sim.printers.length;
 $('printers').innerHTML=sim.printers.map(p=>{const j=p.job,progress=j?Math.min(100,(sim.now-j.start)/(j.pages*200)*100):0;return `<div class="printer-card"><div class="printer-heading"><span class="printer-icon">${printerIcon}</span><div><strong>Printer ${String(p.id).padStart(2,'0')}</strong><small>CONSUMER ${p.id}</small></div><span class="printer-state ${j?'busy':''}">${p.stopped?'ปิดแล้ว':j?'กำลังพิมพ์':'รอรับงาน'}</span></div><div class="printer-job">${j?'#'+j.id+' · '+j.filename:p.stopped?'SHUTDOWN RECEIVED':'— waiting for a job'}</div><div class="progress" role="progressbar" aria-label="ความคืบหน้าเครื่อง ${p.id}" aria-valuenow="${Math.round(progress)}" aria-valuemin="0" aria-valuemax="100"><div style="width:${progress}%"></div></div><div class="printer-meta"><span>${j?`${Math.floor((sim.now-j.start)/200)} / ${j.pages} หน้า`:`พิมพ์เสร็จ ${p.jobs} งาน`}</span><span>${j?Math.floor(progress)+'%':p.pages+' หน้ารวม'}</span></div></div>`;}).join('');
 $('semaphores').innerHTML=[['mutex',1,'ล็อกคิว'],['empty_slots',10-sim.queue.length,'ช่องว่าง'],['filled_slots',sim.queue.length,'งานในคิว'],['stats_lock',1,'ล็อกสถิติ']].map(([n,v,d])=>`<div class="sem"><span>${n}</span><strong>${v}</strong><small>${d}</small></div>`).join('');
 const events=$('events'),nearBottom=events.scrollHeight-events.scrollTop-events.clientHeight<55;
 if(Number(events.dataset.count)!==sim.logs.length){events.innerHTML=sim.logs.length?sim.logs.map(e=>`<div class="event ${e.kind}"><time>${time(e.time)}</time><span class="event-kind">${{enqueue:'QUEUE',print:'PRINT',done:'DONE',wait:'WAIT',system:'SYSTEM'}[e.kind]}</span><span>${e.text}</span></div>`).join(''):'<p class="log-placeholder">เหตุการณ์ของระบบจะแสดงที่นี่…</p>';events.dataset.count=sim.logs.length;if(nearBottom)events.scrollTop=events.scrollHeight;}
 $('event-count').textContent=sim.logs.length+' events';
 const prios=[1,2,3,4,5].map(p=>{const a=sim.dispatched.filter(j=>j.priority===p);return {p,count:a.length,avg:a.length?a.reduce((v,j)=>v+j.wait,0)/a.length:0,max:Math.max(0,...a.map(j=>j.wait))};});
 const maxAvg=Math.max(1000,...prios.map(p=>p.avg));
 $('priority-report').innerHTML='<h4 class="report-heading">เวลารอเฉลี่ยตาม Priority</h4>'+prios.map(p=>`<div class="bar-row" title="${p.count} งาน · รอสูงสุด ${sec(p.max)} วินาที"><span>P${p.p}</span><div class="bar-track"><div class="bar-fill" style="width:${p.avg/maxAvg*100}%"></div></div><span>${p.count?sec(p.avg)+' s':'—'}</span></div>`).join('')+'<table class="data-table"><thead><tr><th>Priority</th><th>จำนวนงาน</th><th>รอสูงสุด</th></tr></thead><tbody>'+prios.map(p=>`<tr><td>P${p.p}</td><td>${p.count}</td><td>${p.count?sec(p.max)+' s':'—'}</td></tr>`).join('')+'</tbody></table>';
 $('printer-report').innerHTML='<h4 class="report-heading">การกระจายงานไปยังเครื่องพิมพ์</h4><table class="data-table"><thead><tr><th>เครื่อง</th><th>เริ่มพิมพ์</th><th>เสร็จแล้ว</th><th>หน้าที่รับ</th></tr></thead><tbody>'+sim.printers.map(p=>{const a=sim.dispatched.filter(j=>j.printer===p.id);return `<tr><td>Printer 0${p.id}</td><td>${a.length} งาน</td><td>${p.jobs} งาน</td><td>${a.reduce((n,j)=>n+j.pages,0)}</td></tr>`;}).join('')+'</tbody></table><div class="run-help">'+(sim.done?`จบการจำลองใน <strong>${sec(sim.now)} วินาที</strong><br>พิมพ์ครบ ${s.completed} งาน รวม ${s.pages} หน้า<br>เริ่มใหม่แล้วเปลี่ยนจำนวนเครื่องหรือ Aging เพื่อเปรียบเทียบ`:'สถิติใน C ถูกบันทึกตอนเริ่มพิมพ์<br>เว็บแยก “เริ่มพิมพ์” กับ “เสร็จแล้ว” ให้เห็นระหว่างทำงาน')+'</div>';
}
function reset(){running=false;sim=new Spooler({printers:Number($('printer-count').value),aging:$('aging').checked});render();}
function toggle(){if(sim.done)return;running=!running;if(running&&!sim.started)sim.step();last=performance.now();render();}
function step(){running=false;sim.step();render();}
function showView(name){if(!['simulator','guide','source'].includes(name))name='simulator';document.querySelectorAll('.view').forEach(v=>v.hidden=v.id!==name);document.querySelectorAll('.tab').forEach(t=>{const active=t.dataset.view===name;t.classList.toggle('active',active);if(active)t.setAttribute('aria-current','page');else t.removeAttribute('aria-current');});if(name==='source')loadSource();}
let sourceLoaded=false;
async function loadSource(){if(sourceLoaded)return;try{const r=await fetch('print_spooler_v2.c');if(!r.ok)throw new Error();const source=await r.text();const container=$('source-lines');if(!container)return;container.replaceChildren(...source.split('\n').map((line,i)=>{const el=document.createElement('span');el.className='code-line'+(/^\s*(\/\*|\*|\/\/)/.test(line)?' code-comment':'');el.id='L'+(i+1);const num=document.createElement('span');num.className='line-num';num.textContent=i+1;el.append(num,document.createTextNode(line));return el;}));sourceLoaded=true;}catch{if($('source-lines'))$('source-lines').textContent='ไม่สามารถโหลดโค้ดได้ กรุณาใช้ลิงก์ดาวน์โหลดด้านบน';}}
document.querySelectorAll('.tab').forEach(b=>b.addEventListener('click',()=>{location.hash=b.dataset.view;showView(b.dataset.view);}));
window.addEventListener('hashchange',()=>{const hash=location.hash.slice(1);if(['simulator','guide','source'].includes(hash))showView(hash);});
$('play').addEventListener('click',toggle);$('step').addEventListener('click',step);$('reset').addEventListener('click',reset);$('printer-count').addEventListener('change',reset);$('aging').addEventListener('change',reset);
let lastRender=0;function frame(now){if(running){sim.advance(Math.min(250,now-last)*Number($('speed').value));if(sim.done)running=false;if(now-lastRender>80||sim.done){render();lastRender=now;}}last=now;requestAnimationFrame(frame);}requestAnimationFrame(frame);
render();
const initialHash=location.hash.slice(1);
showView(initialHash.startsWith('lesson-')?'guide':initialHash);
if(document.modelContext?.registerTool){
 const lifecycle=new AbortController();
 const result=()=>({time_ms:sim.now,status:sim.done?'complete':running?'running':sim.started?'paused':'ready',queue:sim.queue.length,printers:sim.printers.length,aging:sim.aging,...sim.stats});
 try{Promise.resolve(document.modelContext.registerTool({name:'read_spooler_state',title:'อ่านสถานะการจำลอง',description:'Read the current simulated queue, printer settings and statistics.',inputSchema:{type:'object',properties:{},additionalProperties:false},annotations:{readOnlyHint:true,untrustedContentHint:false},execute(input){if(!input||typeof input!=='object'||Object.keys(input).length)throw new Error('Expected an empty object');return result();}},{signal:lifecycle.signal})).catch(()=>{});
 Promise.resolve(document.modelContext.registerTool({name:'step_spooler_simulation',title:'เดินการจำลองทีละขั้น',description:'Pause playback and process the next event time, exactly like the visible step button.',inputSchema:{type:'object',properties:{},additionalProperties:false},annotations:{readOnlyHint:false,untrustedContentHint:false},execute(input){if(!input||typeof input!=='object'||Object.keys(input).length)throw new Error('Expected an empty object');if(sim.done)throw new Error('Simulation is complete; reset in the interface to start again.');step();return result();}},{signal:lifecycle.signal})).catch(()=>{});
 window.addEventListener('pagehide',()=>lifecycle.abort(),{once:true});
 }catch{/* The simulator remains usable when WebMCP is unavailable. */}
}
export {sim,reset,step,showView};
