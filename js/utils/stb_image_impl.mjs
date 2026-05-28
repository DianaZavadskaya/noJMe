/**
 * STB Image Implementation - JavaScript ESM port
 * Migrated from src/utils/stb_image_impl.c (35 lines)
 *
 * Replicated public surface: stbi_load_from_memory, stbi_load,
 *   stbi_image_free, stbi_failure_reason, stbi_info_from_memory
 *
 * Translation decisions:
 *   - PNG: pure-JS using Node.js zlib.inflateSync + filter reconstruction
 *   - JPEG: baseline-DCT pure-JS (8-bit non-progressive YCbCr/grey)
 *   - BMP: pure-JS 24-bpp and 32-bpp uncompressed DIB
 *   - stbi_image_free: no-op (GC handles memory)
 *   - desired_channels=4 (RGBA) is the primary tested path
 */
import { inflateSync } from 'zlib';
import { readFileSync } from 'fs';

// Module-level last-error state (mirrors stbi__g_failure_reason)
let _failureReason = '';
function _fail(msg) { _failureReason = msg; return null; }
export function stbi_failure_reason() { return _failureReason; }

// stbi_image_free: no-op, GC handles deallocation
export function stbi_image_free(_pixels) {}
// stbi_load_from_memory(buffer, len, desired_channels)
// buffer: Uint8Array|Buffer|ArrayBuffer; returns {pixels,width,height,channels}|null
export function stbi_load_from_memory(buffer, len, desired_channels) {
  if (desired_channels == null) desired_channels = 4;
  _failureReason = '';
  let data;
  if (buffer instanceof Uint8Array) {
    data = (len != null && len < buffer.length) ? buffer.subarray(0, len) : buffer;
  } else if (buffer instanceof ArrayBuffer) {
    data = new Uint8Array(buffer, 0, len != null ? len : buffer.byteLength);
  } else if (typeof Buffer !== 'undefined' && Buffer.isBuffer(buffer)) {
    data = new Uint8Array(buffer.buffer, buffer.byteOffset, len != null ? len : buffer.byteLength);
  } else {
    return _fail('stbi_load_from_memory: unsupported buffer type');
  }
  if (data.length < 4) return _fail('not enough data');
  if (_isPng(data))  return _decodePng(data, desired_channels);
  if (_isJpeg(data)) return _decodeJpeg(data, desired_channels);
  if (_isBmp(data))  return _decodeBmp(data, desired_channels);
  return _fail('unknown image format');
}

// stbi_load(filename, desired_channels)
export function stbi_load(filename, desired_channels) {
  if (desired_channels == null) desired_channels = 4;
  _failureReason = '';
  let buf;
  try { buf = readFileSync(filename); }
  catch (e) { return _fail("can't fopen " + filename + ': ' + e.message); }
  return stbi_load_from_memory(
    new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength), buf.byteLength, desired_channels);
}

// stbi_info_from_memory(buffer)
export function stbi_info_from_memory(buffer) {
  if (!buffer || buffer.length < 8) return _fail('not enough data');
  if (_isPng(buffer))  return _pngInfo(buffer);
  if (_isJpeg(buffer)) return _jpegInfo(buffer);
  if (_isBmp(buffer))  return _bmpInfo(buffer);
  return _fail('unknown image format');
}

// --- format detection ---
function _isPng(d)  { return d[0]===0x89&&d[1]===0x50&&d[2]===0x4E&&d[3]===0x47; }
function _isJpeg(d) { return d[0]===0xFF&&d[1]===0xD8; }
function _isBmp(d)  { return d[0]===0x42&&d[1]===0x4D; }

// --- read helpers ---
function _r16BE(d,o)  { return (d[o]<<8)|d[o+1]; }
function _r32BE(d,o)  { return ((d[o]<<24)|(d[o+1]<<16)|(d[o+2]<<8)|d[o+3])>>>0; }
function _r16LE(d,o)  { return (d[o+1]<<8)|d[o]; }
function _r32LE(d,o)  { return ((d[o+3]<<24)|(d[o+2]<<16)|(d[o+1]<<8)|d[o])>>>0; }
function _rs32LE(d,o) { return (d[o+3]<<24)|(d[o+2]<<16)|(d[o+1]<<8)|d[o]; }

// --- channel conversion ---
function _convertChannels(src, sch, dch, w, h) {
  if (sch===dch) return src;
  const n=w*h, dst=new Uint8Array(n*dch);
  for (let i=0; i<n; i++) {
    const si=i*sch, di=i*dch;
    let r,g,b,a;
    if      (sch===1){r=g=b=src[si];a=255;}
    else if (sch===2){r=g=b=src[si];a=src[si+1];}
    else if (sch===3){r=src[si];g=src[si+1];b=src[si+2];a=255;}
    else             {r=src[si];g=src[si+1];b=src[si+2];a=src[si+3];}
    if      (dch===1){dst[di]=(r*0.299+g*0.587+b*0.114+0.5)|0;}
    else if (dch===2){dst[di]=(r*0.299+g*0.587+b*0.114+0.5)|0;dst[di+1]=a;}
    else if (dch===3){dst[di]=r;dst[di+1]=g;dst[di+2]=b;}
    else             {dst[di]=r;dst[di+1]=g;dst[di+2]=b;dst[di+3]=a;}
  }
  return dst;
}
// =========================================================================
// PNG decoder
// =========================================================================

function _pngInfo(d) {
  if (d.length<24) return _fail('PNG: truncated IHDR');
  const ct=d[25], ch=[1,0,3,1,2,0,4][ct]||0;
  if (!ch) return _fail('PNG: unsupported color type '+ct);
  return { width:_r32BE(d,16), height:_r32BE(d,20), channels:ch };
}

function _decodePng(d, desired) {
  let pos=8, ihdr=null;
  const idatParts=[];
  let palette=null, trns=null;
  while (pos+12<=d.length) {
    const cLen=_r32BE(d,pos);
    const cType=String.fromCharCode(d[pos+4],d[pos+5],d[pos+6],d[pos+7]);
    const cData=d.subarray(pos+8, pos+8+cLen);
    pos+=12+cLen;
    if      (cType==='IHDR') ihdr={width:_r32BE(cData,0),height:_r32BE(cData,4),
                                   bitDepth:cData[8],colorType:cData[9],interlace:cData[12]};
    else if (cType==='PLTE') palette=cData.slice();
    else if (cType==='tRNS') trns=cData.slice();
    else if (cType==='IDAT') idatParts.push(cData.slice());
    else if (cType==='IEND') pos=d.length;
  }
  if (!ihdr)             return _fail('PNG: missing IHDR');
  if (ihdr.interlace)    return _fail('PNG: interlaced not supported');
  if (!idatParts.length) return _fail('PNG: no IDAT');
  const {width,height,bitDepth:bd,colorType:ct}=ihdr;
  if (![1,2,4,8,16].includes(bd)) return _fail('PNG: bad bit depth '+bd);
  if (![0,2,3,4,6].includes(ct))  return _fail('PNG: bad color type '+ct);
  const colorCh=[1,0,3,1,2,0,4][ct];
  let cTot=0; for (const p of idatParts) cTot+=p.byteLength;
  const comp=new Uint8Array(cTot); let off=0;
  for (const p of idatParts){comp.set(p,off);off+=p.byteLength;}
  let raw;
  try{raw=inflateSync(comp);}
  catch(e){return _fail('PNG: zlib error: '+e.message);}
  const bpp=Math.max(1,Math.floor(colorCh*bd/8));
  const rowStride=Math.ceil(width*colorCh*bd/8);
  if (raw.length<height*(rowStride+1)) return _fail('PNG: decompressed data too short');
  const pix8=new Uint8Array(height*rowStride);
  let rp=0;
  for (let row=0; row<height; row++) {
    const flt=raw[rp++];
    const rowO=pix8.subarray(row*rowStride,(row+1)*rowStride);
    const prev=row>0?pix8.subarray((row-1)*rowStride,row*rowStride):null;
    for (let i=0; i<rowStride; i++) {
      const x=raw[rp+i];
      const a=i>=bpp?rowO[i-bpp]:0;
      const b=prev?prev[i]:0;
      const c=(prev&&i>=bpp)?prev[i-bpp]:0;
      if      (flt===0) rowO[i]=x;
      else if (flt===1) rowO[i]=(x+a)&0xFF;
      else if (flt===2) rowO[i]=(x+b)&0xFF;
      else if (flt===3) rowO[i]=(x+((a+b)>>1))&0xFF;
      else if (flt===4) rowO[i]=(x+_paeth(a,b,c))&0xFF;
      else return _fail('PNG: unknown filter '+flt);
    }
    rp+=rowStride;
  }
  const nCh=ct===3?4:ct===4?2:ct===6?4:ct===2?3:1;
  const rgba=new Uint8Array(width*height*nCh);
  for (let row=0; row<height; row++) {
    const srcRow=pix8.subarray(row*rowStride,(row+1)*rowStride);
    for (let col=0; col<width; col++) {
      const di=(row*width+col)*nCh;
      if (ct===3) {
        const idx=_rBits(srcRow,col*bd,bd), pi=idx*3;
        rgba[di  ]=palette?palette[pi]  ||0:0;
        rgba[di+1]=palette?palette[pi+1]||0:0;
        rgba[di+2]=palette?palette[pi+2]||0:0;
        rgba[di+3]=trns?(trns[idx]!==undefined?trns[idx]:255):255;
      } else if (bd===16) {
        const si=col*colorCh*2;
        for (let ch=0;ch<colorCh;ch++) rgba[di+ch]=(_r16BE(srcRow,si+ch*2)>>8)&0xFF;
        if (nCh>colorCh) rgba[di+nCh-1]=255;
      } else if (bd<8) {
        const v=_rBits(srcRow,col*bd,bd);
        rgba[di]=Math.round(v*255/((1<<bd)-1));
        if (nCh>1) rgba[di+1]=255;
      } else {
        const si=col*colorCh;
        for (let ch=0;ch<colorCh;ch++) rgba[di+ch]=srcRow[si+ch];
        if (nCh>colorCh) rgba[di+nCh-1]=255;
      }
    }
  }
  const dc=desired===0?nCh:desired;
  return {pixels:_convertChannels(rgba,nCh,dc,width,height),width,height,channels:dc};
}
function _paeth(a,b,c) {
  const p=a+b-c, pa=Math.abs(p-a), pb=Math.abs(p-b), pc=Math.abs(p-c);
  return (pa<=pb&&pa<=pc)?a:(pb<=pc?b:c);
}
function _rBits(src,bitOff,bits) {
  const bi=bitOff>>3, sh=8-(bitOff&7)-bits;
  return (src[bi]>>sh)&((1<<bits)-1);
}
// =========================================================================
// BMP (24-bpp and 32-bpp uncompressed DIB)
// =========================================================================

function _bmpInfo(d) {
  if (d.length<26) return _fail('BMP: truncated');
  const ds=_r32LE(d,14);
  let w,h,bpp;
  if (ds===12){w=_r16LE(d,18);h=_r16LE(d,20);bpp=_r16LE(d,22);}
  else{w=Math.abs(_rs32LE(d,18));h=Math.abs(_rs32LE(d,22));bpp=_r16LE(d,28);}
  return {width:w,height:h,channels:bpp>=32?4:3};
}
function _decodeBmp(d, desired) {
  if (d.length<54) return _fail('BMP: too small');
  const pixOff=_r32LE(d,10), ds=_r32LE(d,14);
  let w,h,bpp,comp,topDown;
  if (ds===12){w=_r16LE(d,18);h=_r16LE(d,20);bpp=_r16LE(d,22);comp=0;topDown=false;}
  else{
    w=Math.abs(_rs32LE(d,18));
    const hr=_rs32LE(d,22);h=Math.abs(hr);topDown=hr<0;
    bpp=_r16LE(d,28);comp=_r32LE(d,30);
  }
  if (![24,32].includes(bpp)) return _fail('BMP: unsupported bpp='+bpp);
  if (comp!==0&&comp!==3)     return _fail('BMP: unsupported compression='+comp);
  const sb=bpp>>3, rs=((w*sb+3)&~3), nCh=bpp===32?4:3;
  const rgba=new Uint8Array(w*h*nCh);
  for (let row=0;row<h;row++) {
    const sr=topDown?row:h-1-row, so=pixOff+sr*rs;
    for (let col=0;col<w;col++) {
      const sp=so+col*sb, dp=(row*w+col)*nCh;
      rgba[dp]=d[sp+2]; rgba[dp+1]=d[sp+1]; rgba[dp+2]=d[sp];
      if (nCh===4) rgba[dp+3]=d[sp+3];
    }
  }
  const dc=desired===0?nCh:desired;
  return {pixels:_convertChannels(rgba,nCh,dc,w,h),width:w,height:h,channels:dc};
}
// =========================================================================
// JPEG (baseline DCT, 8-bit, non-progressive)
// =========================================================================

function _jpegInfo(d) {
  let pos=2;
  while (pos+4<=d.length) {
    if (d[pos]!==0xFF) return _fail('JPEG: lost sync');
    const mk=d[pos+1];
    if (mk===0xC0||mk===0xC1||mk===0xC2)
      return {width:_r16BE(d,pos+7),height:_r16BE(d,pos+5),channels:d[pos+9]};
    pos+=2+_r16BE(d,pos+2);
  }
  return _fail('JPEG: SOF not found');
}
function _decodeJpeg(data, desired) {
  try {
    const r=_jpegDecode(data);
    if (!r) return null;
    const dc=desired===0?r.channels:desired;
    return {pixels:_convertChannels(r.pixels,r.channels,dc,r.width,r.height),
            width:r.width,height:r.height,channels:dc};
  } catch(e){ return _fail('JPEG: '+e.message); }
}
function _jpegDecode(data) {
  const d=data instanceof Uint8Array?data:new Uint8Array(data);
  let pos=0;
  const u8=()=>d[pos++];
  const u16=()=>{const v=_r16BE(d,pos);pos+=2;return v;};
  if (u8()!==0xFF||u8()!==0xD8) return _fail('JPEG: not a JPEG');
  let W=0,H=0,nC=0;
  const comp=[],qt=[null,null,null,null];
  const hDC=[null,null],hAC=[null,null];
  let scanComps=[];
  while (pos+2<=d.length) {
    if (d[pos]!==0xFF){while(pos<d.length&&d[pos]!==0xFF)pos++;continue;}
    while(pos<d.length&&d[pos]===0xFF)pos++;
    if (pos>=d.length) break;
    const mk=u8();
    if (mk===0xD9) break;
    if (mk===0xD8||mk===0x00) continue;
    if (mk>=0xD0&&mk<=0xD7) continue;
    const ss=pos,sl=u16(),se=ss-2+sl;
    if (mk===0xC0||mk===0xC1){
      u8();H=u16();W=u16();nC=u8();comp.length=0;
      for(let i=0;i<nC;i++){const id=u8(),hv=u8(),q=u8();comp.push({id,h:(hv>>4)&0xF,v:hv&0xF,qtId:q});}
    } else if (mk===0xC2){
      return _fail('JPEG: progressive not supported');
    } else if (mk===0xC4){
      let lp=pos;
      while(lp<se){
        const tc=(d[lp]>>4)&1,th=d[lp]&0xF;lp++;
        const bits=new Array(17).fill(0);let tot=0;
        for(let i=1;i<=16;i++){bits[i]=d[lp++];tot+=bits[i];}
        const hv=[];for(let i=0;i<tot;i++)hv.push(d[lp++]);
        const t=_buildHuff(bits,hv);
        if(tc===0)hDC[th]=t;else hAC[th]=t;
      }
    } else if (mk===0xDB){
      let lp=pos;
      while(lp<se){
        const pq=(d[lp]>>4)&0xF,tq=d[lp]&0xF;lp++;
        const t=new Int32Array(64);
        if(pq===0)for(let i=0;i<64;i++)t[i]=d[lp++];
        else for(let i=0;i<64;i++){t[i]=_r16BE(d,lp);lp+=2;}
        qt[tq]=t;
      }
    } else if (mk===0xDA){
      const ns=u8();scanComps=[];
      for(let i=0;i<ns;i++){
        const id=u8(),td=(d[pos]>>4)&0xF,ta=d[pos]&0xF;pos++;
        const c=comp.find(x=>x.id===id);
        if(!c) return _fail('JPEG: unknown component');
        scanComps.push({c,dcId:td,acId:ta});
      }
      u8();u8();u8();
      const ec0=pos;let ec1=ec0;
      while(ec1+1<d.length){
        if(d[ec1]===0xFF&&d[ec1+1]!==0x00&&!(d[ec1+1]>=0xD0&&d[ec1+1]<=0xD7))break;
        ec1++;
      }
      return _scanDecode(d,ec0,ec1,W,H,nC,comp,qt,hDC,hAC,scanComps);
    }
    pos=se;
  }
  return _fail('JPEG: no SOS');
}

function _buildHuff(bits, vals) {
  const t = { fast: new Int16Array(256).fill(-1), fastLen: new Uint8Array(256), codes: [] };
  let code = 0, vi = 0;
  for (let len = 1; len <= 16; len++) {
    for (let j = 0; j < bits[len]; j++) {
      const v = vals[vi++];
      t.codes.push({ code, len, v });
      if (len <= 8) {
        const base = code << (8 - len);
        const cnt = 1 << (8 - len);
        for (let k = 0; k < cnt; k++) {
          t.fast[base + k] = v;
          t.fastLen[base + k] = len;
        }
      }
      code++;
    }
    code <<= 1;
  }
  return t;
}

class BitStream {
  constructor(data, start, end) {
    this.d = data;
    this.pos = start;
    this.end = end;
    this.buf = 0;
    this.bits = 0;
  }
  _fill() {
    while (this.bits <= 24) {
      if (this.pos >= this.end) { this.buf |= 0xFF << (24 - this.bits); this.bits += 8; continue; }
      let b = this.d[this.pos++];
      if (b === 0xFF) {
        const nx = this.d[this.pos];
        if (nx === 0x00) { this.pos++; }
        else { b = 0xFF; }
      }
      this.buf |= b << (24 - this.bits);
      this.bits += 8;
    }
  }
  rBits(n) {
    if (this.bits < n) this._fill();
    const v = (this.buf >>> (32 - n)) & ((1 << n) - 1);
    this.buf <<= n;
    this.bits -= n;
    return v;
  }
  peek8() {
    if (this.bits < 8) this._fill();
    return (this.buf >>> 24) & 0xFF;
  }
  skip(n) {
    if (this.bits < n) this._fill();
    this.buf <<= n;
    this.bits -= n;
  }
  decH(t) {
    if (this.bits < 8) this._fill();
    const p8 = (this.buf >>> 24) & 0xFF;
    const fv = t.fast[p8];
    if (fv >= 0) {
      this.skip(t.fastLen[p8]);
      return fv;
    }
    let code = 0;
    for (let len = 1; len <= 16; len++) {
      code = (code << 1) | this.rBits(1);
      for (const e of t.codes) {
        if (e.len === len && e.code === code) return e.v;
      }
    }
    return -1;
  }
  rcvExt(s) {
    if (s === 0) return 0;
    const v = this.rBits(s);
    if (v < (1 << (s - 1))) return v - (1 << s) + 1;
    return v;
  }
}
const ZZ = new Uint8Array([
   0, 1, 8,16, 9, 2, 3,10,
  17,24,32,25,18,11, 4, 5,
  12,19,26,33,40,48,41,34,
  27,20,13, 6, 7,14,21,28,
  35,42,49,56,57,50,43,36,
  29,22,15,23,30,37,44,51,
  58,59,52,45,38,31,39,46,
  53,60,61,54,47,55,62,63
]);

const W1=2841,W2=2676,W3=2408,W5=1609,W6=1108,W7=565;

function _idctR(b, o) {
  const x0=b[o]<<11, x1=b[o+4]<<11;
  const x2=b[o+6], x3=b[o+2];
  const x4=b[o+1], x5=b[o+7];
  const x6=b[o+5], x7=b[o+3];
  let tmp;
  let t0=x0+x1, t1=x0-x1;
  let t2=(W6*x2-W2*x3+128)>>8, t3=(W2*x2+W6*x3+128)>>8;
  let t4=((W7*x4-W1*x5+128)>>8), t5=((W1*x4+W7*x5+128)>>8);
  let t6=((W3*x6-W5*x7+128)>>8), t7=((W5*x6+W3*x7+128)>>8);
  let p=t0+t3, q=t1+t2, r=t1-t2, s=t0-t3;
  tmp=t4+t6; const u=t5+t7; const v=t4-t6; const w=t5-t7;
  t4=(tmp+u+128)>>8; t6=(tmp-u+128)>>8;
  t5=((v+w)*181+128)>>8; t7=((v-w)*181+128)>>8;
  b[o+0]=(p+t5)>>3; b[o+1]=(q+t4)>>3;
  b[o+2]=(r+t7)>>3; b[o+3]=(s+t6)>>3;
  b[o+4]=(s-t6)>>3; b[o+5]=(r-t7)>>3;
  b[o+6]=(q-t4)>>3; b[o+7]=(p-t5)>>3;
}

function _idctC(b, o) {
  const x0=(b[o]<<8)+8192, x1=b[o+4*8]<<8;
  const x2=b[o+6*8], x3=b[o+2*8];
  const x4=b[o+1*8], x5=b[o+7*8];
  const x6=b[o+5*8], x7=b[o+3*8];
  const t0=x0+x1, t1=x0-x1;
  const t2=(W6*x2-W2*x3+128)>>8, t3=(W2*x2+W6*x3+128)>>8;
  let t4=(W7*x4-W1*x5+128)>>8, t5=(W1*x4+W7*x5+128)>>8;
  let t6=(W3*x6-W5*x7+128)>>8, t7=(W5*x6+W3*x7+128)>>8;
  const p=t0+t3, q=t1+t2, r=t1-t2, s=t0-t3;
  const tmp2=t4+t6; const u=t5+t7; const v=t4-t6; const w=t5-t7;
  t4=(tmp2+u+128)>>8; t6=(tmp2-u+128)>>8;
  t5=((v+w)*181+128)>>8; t7=((v-w)*181+128)>>8;
  b[o+0*8]=(p+t5)>>14; b[o+1*8]=(q+t4)>>14;
  b[o+2*8]=(r+t7)>>14; b[o+3*8]=(s+t6)>>14;
  b[o+4*8]=(s-t6)>>14; b[o+5*8]=(r-t7)>>14;
  b[o+6*8]=(q-t4)>>14; b[o+7*8]=(p-t5)>>14;
}

function _idct8x8(b) {
  for (let i = 0; i < 8; i++) _idctR(b, i * 8);
  for (let i = 0; i < 8; i++) _idctC(b, i);
}

function _cl(v) { return v < 0 ? 0 : v > 255 ? 255 : v | 0; }
function _clI(v) { return v < -128 ? -128 : v > 127 ? 127 : v | 0; }

function _gS(st, ci, col, row, mH, mV) {
  const c = st[ci];
  const sx = Math.floor(col * c.sfH / mH);
  const sy = Math.floor(row * c.sfV / mV);
  return c.data[sy * c.stride + sx];
}
function _scanDecode(d, ec0, ec1, W, H, nC, comp, qt, hDC, hAC, scanComps) {
  const bs = new BitStream(d, ec0, ec1);
  const mH = Math.max(...comp.map(c => c.sfH));
  const mV = Math.max(...comp.map(c => c.sfV));
  const mcuW = mH * 8, mcuH = mV * 8;
  const mcuCols = Math.ceil(W / mcuW);
  const mcuRows = Math.ceil(H / mcuH);

  for (const c of comp) {
    c.stride = Math.ceil(W * c.sfH / mH / 8) * 8;
    c.data = new Int16Array(c.stride * (Math.ceil(H * c.sfV / mV / 8) * 8));
  }

  const dcPred = new Int16Array(nC);
  const blk = new Int16Array(64);

  for (let mr = 0; mr < mcuRows; mr++) {
    for (let mc = 0; mc < mcuCols; mc++) {
      for (const sc of scanComps) {
        const c = sc.c;
        const dcT = hDC[sc.dcId];
        const acT = hAC[sc.acId];
        for (let bv = 0; bv < c.sfV; bv++) {
          for (let bh = 0; bh < c.sfH; bh++) {
            blk.fill(0);
            const s = bs.decH(dcT);
            const diff = bs.rcvExt(s);
            dcPred[c.idx] += diff;
            blk[0] = dcPred[c.idx] * qt[c.qtId][0];
            let k = 1;
            while (k < 64) {
              const rs = bs.decH(acT);
              if (rs < 0) break;
              if (rs === 0) break;
              if (rs === 0xF0) { k += 16; continue; }
              const run = (rs >> 4) & 0xF;
              const sz = rs & 0xF;
              k += run;
              if (k >= 64) break;
              blk[ZZ[k]] = bs.rcvExt(sz) * qt[c.qtId][k];
              k++;
            }
            _idct8x8(blk);
            const bx = (mc * c.sfH + bh) * 8;
            const by = (mr * c.sfV + bv) * 8;
            for (let y = 0; y < 8; y++) {
              for (let x = 0; x < 8; x++) {
                const px = bx + x, py = by + y;
                if (px < c.stride)
                  c.data[py * c.stride + px] = _clI(blk[y * 8 + x]);
              }
            }
          }
        }
      }
    }
  }

  const channels = 4;
  const out = new Uint8Array(W * H * channels);
  for (let row = 0; row < H; row++) {
    for (let col = 0; col < W; col++) {
      const i = (row * W + col) * 4;
      if (nC === 1) {
        const y = _gS(comp, 0, col, row, mH, mV) + 128;
        out[i] = _cl(y); out[i+1] = _cl(y); out[i+2] = _cl(y); out[i+3] = 255;
      } else {
        const Y  = _gS(comp, 0, col, row, mH, mV) + 128;
        const Cb = _gS(comp, 1, col, row, mH, mV);
        const Cr = _gS(comp, 2, col, row, mH, mV);
        const r = Y + ((Cr * 91750) >> 16);
        const g = Y - ((Cb * 22554 + Cr * 46802) >> 16);
        const b = Y + ((Cb * 116130) >> 16);
        out[i] = _cl(r); out[i+1] = _cl(g); out[i+2] = _cl(b); out[i+3] = 255;
      }
    }
  }
  return { pixels: out, width: W, height: H, channels };
}
