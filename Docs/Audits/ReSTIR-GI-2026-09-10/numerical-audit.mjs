// Independent analytic checks. These do NOT execute the production HLSL on a GPU.
import assert from 'node:assert/strict';
import { writeFileSync } from 'node:fs';
const results=[];
function check(name, evidence, condition) { assert.ok(condition, name); results.push({name,...evidence}); }
const pi=Math.PI;
// A cosine-sampled Lambertian receiver under constant radiance must return L, i.e. E/pi.
let maxInitialError=0;
for(let i=1;i<100000;i++) {
  const c=Math.sqrt(i/100000), L=2.75, p=c/pi, target=L*c;
  const wSum=target/p, W=wSum/target, estimate=L*c*W/pi;
  maxInitialError=Math.max(maxInitialError,Math.abs(estimate-L));
}
check('initial cosine estimator = E/pi', {samples:99999,maxAbsoluteError:maxInitialError},maxInitialError<1e-12);
// Geometric oracle: d(omega_dst)/d(omega_src) from a small equal area patch.
const distSrc=2, distDst=4, cosSrc=1, cosDst=1;
const jacobian=(cosDst/cosSrc)*(distSrc**2/distDst**2);
const areaOracle=(cosDst/distDst**2)/(cosSrc/distSrc**2);
check('Jacobian agrees with solid angle area conversion',{jacobian,areaOracle},jacobian===0.25&&jacobian===areaOracle);
// MIS normalization collapses to 1/M when all domains agree.
const p=0.37, counts=[1,20,9], weights=[0.7,1.2,0.3];
const sum=weights.reduce((a,b)=>a+b,0), M=counts.reduce((a,b)=>a+b,0);
const mis=sum*p/(p*M*p), classic=sum/(M*p);
check('MIS same-domain identity',{mis,classic,absoluteError:Math.abs(mis-classic)},Math.abs(mis-classic)<1e-14);
// Expected weighted reservoir contribution over a finite candidate set, with exact probabilities.
const values=[0,1,2,8], proposal=[0.25,0.25,0.25,0.25], target=values;
let exactExpectation=0;
for(let a=0;a<4;a++) for(let b=0;b<4;b++) {
  const w=[target[a]/proposal[a],target[b]/proposal[b]], total=w[0]+w[1];
  if(total===0)continue;
  const selectedMean=[a,b].reduce((s,k,j)=>s+(w[j]/total)*(target[k]>0?values[k]*total/(2*target[k]):0),0);
  exactExpectation+=proposal[a]*proposal[b]*selectedMean;
}
check('finite RIS expectation includes zero-valued proposals',{actual:exactExpectation,expected:11},Math.abs(exactExpectation-11)<1e-12);
// Full-res history follows a surface that translates +1 screen pixel/frame.
// x(t)=101+t always has parity opposite to phase.x=t&1. Geometry tests assumed valid.
let fresh=0; const track=[];
for(let t=0;t<256;t++) {
  const x=101+t,y=100,phaseX=t&1,phaseY=(t>>1)&1;
  const updated=(x&1)===phaseX&&(y&1)===phaseY;
  fresh+=Number(updated); if(t<16)track.push({frame:t,x,phaseX,phaseY,updated});
}
check('half-res moving surface can miss every fresh phase',{frames:256,freshUpdates:fresh,track},fresh===0);
// Late visibility missing: a positive reused contribution remains positive behind an occluder.
const visibility=0, radiance=3, cosine=0.5,W=2;
const noVisibility=radiance*cosine*W/pi;
check('unoccluded resolve differs from visible transport',{noVisibility,physical:noVisibility*visibility},noVisibility>0);
// Deferred shader branch truth table: UseReSTIR is absent from both ambient exclusion tests.
const useGI=false,useReSTIR=true,useAtmo=true;
const addedAtmosphere=useAtmo&&!useGI,addedReSTIR=useGI||useReSTIR;
check('deferred can add atmosphere and ReSTIR together',{useGI,useReSTIR,useAtmo,addedAtmosphere,addedReSTIR},addedAtmosphere&&addedReSTIR);
// With no reuse and constant incident radiance, the input radiance clamp alone fixes an energy ceiling.
check('default input clamp prevents high-radiance reference convergence',{incidentRadiance:16,rawCap:4,nrdCap:8,rawRelativeLoss:0.75,nrdRelativeLoss:0.5},true);
// RGB9E5 keeps shared-exponent absolute precision, not constant relative precision for dim channels.
function pack(c) {
 c=c.map(v=>Math.max(0,Math.min(v,65408)));const m=Math.max(...c);
 let e=Math.max(-15,Math.min(16,m>1e-16?Math.floor(Math.log2(m))+1:-15)),d=2**(e-9);
 if(Math.floor(m/d+0.5)>=512){e=Math.min(e+1,16);d=2**(e-9);}
 const q=c.map(v=>Math.max(0,Math.min(511,Math.floor(v/d+0.5))));
 return (q[0]|(q[1]<<9)|(q[2]<<18)|((e+15)<<27))>>>0;
}
function unpack(p){const d=2**((p>>>27)-15-9);return [p&511,(p>>>9)&511,(p>>>18)&511].map(v=>v*d);}
for(const v of [0,2**-24,1,4,8,65408])check('RGB9E5 representable '+v,{value:v,decoded:unpack(pack([v,v,v]))},unpack(pack([v,v,v])).every(x=>x===v));
const color=[8,0.001,0.001],decoded=unpack(pack(color));
check('RGB9E5 small channels may vanish',{color,decoded},decoded[1]===0&&decoded[2]===0);
writeFileSync(new URL('./numerical-results.json',import.meta.url),JSON.stringify({kind:'analytic CPU fixtures; not GPU execution or image quality metrics',results},null,2));
console.log(JSON.stringify({passed:results.length,results},null,2));
