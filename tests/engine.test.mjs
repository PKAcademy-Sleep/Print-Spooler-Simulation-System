import test from 'node:test';
import assert from 'node:assert/strict';
import {Spooler,effective,bestIndex} from '../dist/engine.mjs';
test('aging boundaries, unbounded priority and sentinel immunity',()=>{
 const job={priority:1,enqueue:0};
 assert.equal(effective(job,1999,true),1);assert.equal(effective(job,2000,true),0);
 assert.equal(effective(job,4000,true),-1);assert.equal(effective(job,6000,true),-2);
 assert.equal(effective(job,12000,true),-5);assert.equal(effective(job,12000,false),1);
 assert.equal(effective({id:-1,priority:-1,enqueue:0},100000,true),Infinity);
 assert.equal(effective({id:-1,priority:-1,enqueue:0},100000,false),Infinity);
});
test('selection honors effective priority, older timestamps and exact-tie array order',()=>{
 const queue=[{id:101,priority:1,enqueue:8000},{id:102,priority:5,enqueue:0},{id:-1,priority:-1,enqueue:0}];
 assert.equal(bestIndex(queue,8000,true),1);assert.equal(bestIndex(queue,8000,false),0);
 assert.equal(bestIndex([{priority:3,enqueue:0},{priority:3,enqueue:0}],0,true),0);
});
test('lower priority numbers win and shutdown follows real jobs even with negative effective priority',()=>{
 const shutdown={id:-1,priority:-1,enqueue:0};
 const queue=[shutdown,{id:100,priority:5,enqueue:6000},{id:101,priority:1,enqueue:6000}];
 assert.equal(bestIndex(queue,6000,false),2);
 for(const now of [4000,6000]) {
  const real={id:100,priority:1,enqueue:0};
  assert.equal(bestIndex([shutdown,real],now,true),1);
  assert.equal(bestIndex([real,shutdown],now,true),0);
 }
 assert.equal(bestIndex([shutdown,{...shutdown,enqueue:1000}],10000,true),0);
});
test('a printer can stop while another finishes an aged job without losing work',()=>{
 const sim=new Spooler({printers:2,aging:true});
 sim.now=6000;sim.producers.forEach(p=>{p.done=true;p.index=5;});sim.shutdown=true;sim.sentinels=2;
 const shutdown={id:-1,priority:-1,pages:0,filename:'SHUTDOWN',enqueue:0};
 sim.queue=[shutdown,{id:100,priority:1,pages:1,filename:'aged.txt',enqueue:0},{...shutdown}];
 sim.settle();
 assert.equal(sim.dispatched.length,1);assert.equal(sim.dispatched[0].eff,-2);
 assert.equal(sim.done,false);assert.equal(sim.printers.filter(p=>p.stopped).length,1);
 sim.advance(200);
 assert.equal(sim.done,true);assert.deepEqual(sim.completed.map(j=>j.id),[100]);
 assert.equal(sim.queue.length,0);assert.ok(sim.printers.every(p=>p.stopped));assert.equal(sim.stats.aged,1);
});
for(const printers of [1,2,3,4])for(const aging of [false,true])test(`${printers} printers, aging ${aging}: capacity, unique jobs, shutdown, statistics`,()=>{
 const sim=new Spooler({printers,aging});let ticks=0,maxQueue=0;
 while(!sim.done&&ticks++<500){sim.step();maxQueue=Math.max(maxQueue,sim.queue.length);assert.ok(sim.queue.length<=10);assert.ok(sim.queue.length>=0);assert.ok(sim.completed.length<=sim.dispatched.length);for(const j of sim.dispatched){assert.ok(j.wait>=0);assert.ok(j.priority>=1&&j.priority<=5);}}
 assert.ok(sim.done);assert.equal(sim.completed.length,20);assert.equal(new Set(sim.completed.map(j=>j.id)).size,20);
 assert.equal(sim.sentinels,printers);assert.equal(sim.queue.length,0);assert.ok(sim.printers.every(p=>p.stopped));
 assert.equal(sim.stats.started,20);assert.equal(sim.stats.pages,sim.producers.flatMap(p=>p.jobs).reduce((n,j)=>n+j.pages,0));
 assert.equal(sim.stats.average,sim.dispatched.reduce((n,j)=>n+j.wait,0)/20);
 assert.equal(sim.stats.aged,sim.dispatched.filter(j=>j.eff<j.priority).length);
 assert.ok(sim.dispatched.every(j=>j.id!==-1));
 assert.ok(sim.completed.every(j=>j.finished-j.start===j.pages*200));
 if(!aging){assert.equal(sim.stats.aged,0);assert.ok(sim.dispatched.every(j=>j.eff===j.priority));}
 if(printers===1){assert.equal(maxQueue,10);assert.ok(sim.logs.some(e=>e.kind==='wait'));if(aging)assert.ok(sim.stats.aged>0);}
});
test('stepping and playback produce identical schedule and timing',()=>{
 const a=new Spooler(),b=new Spooler();while(!a.done)a.step();while(!b.done)b.advance(37);
 assert.deepEqual(a.dispatched,b.dispatched);assert.equal(a.now,b.now);assert.deepEqual(a.completed,b.completed);
});
test('simulation is repeatable and no work or timestamps change after completion',()=>{
 const a=new Spooler(),b=new Spooler();while(!a.done)a.step();while(!b.done)b.step();assert.deepEqual(a.logs,b.logs);
 const logLength=a.logs.length,now=a.now;a.step();a.advance(5000);assert.equal(a.logs.length,logLength);assert.equal(a.now,now);
});
test('invalid printer counts are rejected',()=>{for(const printers of [0,5,1.5])assert.throws(()=>new Spooler({printers}));});
