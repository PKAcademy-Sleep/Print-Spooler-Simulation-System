export const CAPACITY = 10;
export const AGING_MS = 2000;
export const PAGE_MS = 200;
export function effective(job, now, aging) {
  return job.id === -1 ? Infinity : job.priority - (aging ? Math.floor((now-job.enqueue)/AGING_MS) : 0);
}
// Tie-break: earlier enqueue time wins; if the timestamps match to the ms, the lower arrival sequence wins (true FIFO).
function olderThan(x, y) {
  return x.enqueue<y.enqueue || (x.enqueue===y.enqueue && (x.enqueueSequence??0)<(y.enqueueSequence??0));
}
export function bestIndex(queue, now, aging) {
  let best = 0;
  for (let i=1;i<queue.length;i++) {
    const a=effective(queue[i],now,aging), b=effective(queue[best],now,aging);
    if (a<b || (a===b && olderThan(queue[i],queue[best]))) best=i;
  }
  return best;
}
// Presentation order only; keep the storage array and C scheduling rule intact.
export function arrivalOrder(queue) {
  return [...queue].sort((a,b)=>a.enqueue-b.enqueue || (a.enqueueSequence??0)-(b.enqueueSequence??0));
}
export function selectionOrder(queue, now, aging) {
  const pending=[...queue], ordered=[];
  while(pending.length) {
    const index=bestIndex(pending,now,aging);
    ordered.push(pending[index]);
    pending[index]=pending[pending.length-1];pending.pop();
  }
  return ordered;
}
function random(seed) { return () => { seed = (Math.imul(seed,1664525)+1013904223)>>>0; return seed/4294967296; }; }
export class Spooler {
  constructor({printers=1, aging=true, seed=42}={}) {
    if (!Number.isInteger(printers)||printers<1||printers>4) throw new Error('จำนวนเครื่องพิมพ์ต้องเป็น 1–4');
    this.now=0; this.aging=Boolean(aging); this.seed=seed; this.started=false; this.done=false;
    this.queue=[]; this.logs=[]; this.completed=[]; this.dispatched=[]; this.nextEnqueueSequence=0;
    this.sentinels=0; this.shutdown=false;
    const rng=random(seed);
    this.producers=Array.from({length:4},(_,p)=>({id:p+1,index:0,due:0,done:false,blocked:false,jobs:Array.from({length:5},(_,i)=>({id:(p+1)*100+i,producer:p+1,priority:1+Math.floor(rng()*5),pages:1+Math.floor(rng()*8),filename:`user${p+1}_doc${i}.txt`,delay:100+Math.floor(rng()*400)}))}));
    this.printers=Array.from({length:printers},(_,i)=>({id:i+1,job:null,until:Infinity,stopped:false,jobs:0,pages:0}));
  }
  log(kind,text,job=null) {this.logs.push({time:this.now,kind,text,job:job?.id});}
  settle() {
    this.started=true;
    for (const p of this.printers) {
      if (p.job && p.until<=this.now) {
        this.completed.push({...p.job,finished:this.now});
        this.log('done',`เครื่อง ${p.id} พิมพ์ #${p.job.id} เสร็จ (${p.job.pages} หน้า)`,p.job);
        p.jobs++; p.pages+=p.job.pages; p.job=null; p.until=Infinity;
      }
    }
    let changed;
    do {
      changed=false;
      for (const p of this.producers) {
        if (p.done || p.due>this.now) continue;
        if (p.index===5) {p.done=true; p.blocked=false; this.log('system',`Producer ${p.id} ส่งครบ 5 งานและจบการทำงาน`); changed=true; continue;}
        if (this.queue.length>=CAPACITY) {
          if (!p.blocked) this.log('wait',`Producer ${p.id} รอ empty_slots: คิวเต็ม 10 ช่อง`);
          p.blocked=true; continue;
        }
        const job={...p.jobs[p.index],enqueue:this.now,enqueueSequence:this.nextEnqueueSequence++};
        this.queue.push(job); p.index++; p.blocked=false; p.due=this.now+job.delay;
        this.log('enqueue',`Producer ${p.id} ส่ง #${job.id} · P${job.priority} · ${job.pages} หน้า`,job); changed=true;
      }
      if (!this.shutdown && this.producers.every(p=>p.done)) {this.shutdown=true;this.log('system','MAIN: Producer จบครบ เตรียมส่ง SHUTDOWN หนึ่งงานต่อเครื่อง');}
      while (this.shutdown && this.sentinels<this.printers.length && this.queue.length<CAPACITY) {
        this.queue.push({id:-1,priority:-1,pages:0,filename:'SHUTDOWN',enqueue:this.now,enqueueSequence:this.nextEnqueueSequence++});
        this.sentinels++;this.log('system',`MAIN: ส่ง SHUTDOWN ${this.sentinels}/${this.printers.length}`);changed=true;
      }
      for (const p of this.printers) {
        if (p.job || p.stopped || !this.queue.length) continue;
        const index=bestIndex(this.queue,this.now,this.aging), job=this.queue[index];
        this.queue[index]=this.queue[this.queue.length-1];this.queue.pop();changed=true;
        if (job.id===-1) {p.stopped=true;this.log('system',`เครื่อง ${p.id} รับ SHUTDOWN และหยุดทำงาน`);continue;}
        const dispatched={...job,wait:this.now-job.enqueue,eff:effective(job,this.now,this.aging),printer:p.id,start:this.now};
        this.dispatched.push(dispatched);p.job=dispatched;p.until=this.now+job.pages*PAGE_MS;
        this.log('print',`เครื่อง ${p.id} เลือก #${job.id} · P${job.priority}${dispatched.eff<job.priority?' → '+dispatched.eff+' (Aging)':''} · รอ ${dispatched.wait} ms`,job);
      }
    } while(changed);
    if(this.printers.every(p=>p.stopped)) {this.done=true;this.log('system','MAIN: Printer จบครบ สรุปสถิติและคืนทรัพยากร');}
  }
  nextTime() {
    return Math.min(...this.printers.filter(p=>p.job).map(p=>p.until),...this.producers.filter(p=>!p.done&&!p.blocked).map(p=>p.due));
  }
  step() {
    if(this.done) return;
    if(!this.started) {this.settle();return;}
    const next=this.nextTime();
    if(!Number.isFinite(next)) throw new Error('Simulation stalled');
    this.now=next;this.settle();
  }
  advance(ms) {
    if(this.done)return;
    if(!this.started)this.settle();
    const target=this.now+ms;
    while(!this.done && this.nextTime()<=target) {this.now=this.nextTime();this.settle();}
    if(!this.done)this.now=target;
  }
  get stats() {
    const jobs=this.dispatched;
    return {started:jobs.length,completed:this.completed.length,average:jobs.length?jobs.reduce((s,j)=>s+j.wait,0)/jobs.length:0,max:Math.max(0,...jobs.map(j=>j.wait)),aged:jobs.filter(j=>j.eff<j.priority).length,pages:this.completed.reduce((s,j)=>s+j.pages,0)};
  }
}
