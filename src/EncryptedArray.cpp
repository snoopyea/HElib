/* Copyright (C) 2012,2013 IBM Corp.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
/* EncryptedArray.cpp - Data-movement operations on arrays of slots
 */
#include "NTL/ZZ.h"
NTL_CLIENT
#include "EncryptedArray.h"
#include "timing.h"
#include "cloned_ptr.h"

#include <algorithm>


EncryptedArrayBase* buildEncryptedArray(const FHEcontext& context, const ZZX& G)
{
  switch (context.alMod.getTag()) {
    case PA_GF2_tag: {
      return new EncryptedArrayDerived<PA_GF2>(context, conv<GF2X>(G));
    }

    case PA_zz_p_tag: {
      zz_pBak bak; bak.save(); context.alMod.restoreContext();
      return new EncryptedArrayDerived<PA_zz_p>(context, conv<zz_pX>(G));
    }

    default: return 0;
  }
}


template<class type>
EncryptedArrayDerived<type>::EncryptedArrayDerived(const FHEcontext& _context, 
                                           const RX& _G)
: context(_context)

{
  const PAlgebraModDerived<type>& tab = context.alMod.getDerived(type());
  tab.genMaskTable();
  tab.mapToSlots(mappingData, _G); // Compute the base-G representation maps
}



// rotate ciphertext in dimension i by amt
template<class type>
void EncryptedArrayDerived<type>::rotate1D(Ctxt& ctxt, long i, long amt, bool dc) const
{
  FHE_TIMER_START;
  const PAlgebra& al = context.zMStar;

  const PAlgebraModDerived<type>& tab = context.alMod.getDerived(type());

  const vector< vector< RX > >& maskTable = tab.getMaskTable();

  RBak bak; bak.save(); tab.restoreContext();

  assert(&context == &ctxt.getContext());
  assert(i >= 0 && i < (long)al.numOfGens());

  // Make sure amt is in the range [1,ord-1]
  long ord = al.OrderOf(i);
  amt %= ord;
  if (amt == 0) return;
  long signed_amt = amt;
  if (amt < 0) amt += ord;

  // DIRT: the above assumes division with remainder
  // follows C++11 and C99 rules

  if (al.SameOrd(i)) { // a "native" rotation
    long val = PowerMod(al.ZmStarGen(i), amt, al.getM());
    ctxt.smartAutomorph(val);
  }
  else if (dc) { 
    // the "don't care" case...it is presumed that any shifts
    // "off the end" are zero.  For this, we have to use 
    // the "signed" version of amt.
    long val = PowerMod(al.ZmStarGen(i), signed_amt, al.getM());
    ctxt.smartAutomorph(val);
  }
  else {
    // more expensive "non-native" rotation
    assert(maskTable[i].size() > 0);
    long val = PowerMod(al.ZmStarGen(i), amt, al.getM());
    long ival = PowerMod(al.ZmStarGen(i), amt-ord, al.getM());

    const RX& mask = maskTable[i][ord-amt];
    DoubleCRT m1(conv<ZZX>(mask), context, ctxt.getPrimeSet());
    Ctxt tmp(ctxt); // a copy of the ciphertext

    tmp.multByConstant(m1);    // only the slots in which m1=1
    ctxt -= tmp;               // only the slots in which m1=0
    ctxt.smartAutomorph(val);  // shift left by val
    tmp.smartAutomorph(ival);  // shift right by ord-val
    ctxt += tmp;               // combine the two parts
  }
  FHE_TIMER_STOP;
}

// Shift k positions along the i'th dimension with zero fill.
// Negative shift amount denotes shift in the opposite direction.
template<class type>
void EncryptedArrayDerived<type>::shift1D(Ctxt& ctxt, long i, long k) const
{
  FHE_TIMER_START;
  const PAlgebra& al = context.zMStar;

  const PAlgebraModDerived<type>& tab = context.alMod.getDerived(type());

  const vector< vector< RX > >& maskTable = tab.getMaskTable();

  RBak bak; bak.save(); tab.restoreContext();

  assert(&context == &ctxt.getContext());
  assert(i >= 0 && i < (long)al.numOfGens());

  long ord = al.OrderOf(i);

  if (k <= -ord || k >= ord) {
    ctxt.multByConstant(to_ZZX(0));
    return;
  }

  // Make sure amt is in the range [1,ord-1]
  long amt = k % ord;
  if (amt == 0) return;
  if (amt < 0) amt += ord;

  RX mask = maskTable[i][ord-amt];

  long val;
  if (k < 0)
    val = PowerMod(al.ZmStarGen(i), amt-ord, al.getM());
  else {
    mask = 1 - mask;
    val = PowerMod(al.ZmStarGen(i), amt, al.getM());
  }
  DoubleCRT m1(conv<ZZX>(mask), context, ctxt.getPrimeSet());
  ctxt.multByConstant(m1);   // zero out slots where mask=0
  ctxt.smartAutomorph(val);  // shift left by val
  FHE_TIMER_STOP;
}

template<class type>
void EncryptedArrayDerived<type>::rotate(Ctxt& ctxt, long amt) const
{
  FHE_TIMER_START;

  const PAlgebra& al = context.zMStar;

  const PAlgebraModDerived<type>& tab = context.alMod.getDerived(type());

  const vector< vector< RX > >& maskTable = tab.getMaskTable();

  RBak bak; bak.save(); tab.restoreContext();

  assert(&context == &ctxt.getContext());

  // Simple case: just one generator
  if (al.numOfGens()==1) { // VJS: bug fix: <= must be ==
    rotate1D(ctxt, 0, amt);
    return;
  }

  // Make sure that amt is in [1,nslots-1]
  amt %= (long) al.getNSlots();
  if (amt == 0) return;
  if (amt < 0) amt += al.getNSlots();

  // rotate the ciphertext, one dimension at a time
  long i = al.numOfGens()-1;
  long v = al.coordinate(i, amt);
  RX mask = maskTable[i][v];
  Ctxt tmp(ctxt.getPubKey());
  const RXModulus& PhimXmod = tab.getPhimXMod();

  // optimize for the common case where the last generator has order in
  // Zm*/(p) different than its order in Zm*. In this case we can combine
  // the rotate1D relative to this generator with the masking after the
  // rotation. This saves one mult-by-constant, since we use the same mask
  // inside rotate1D as in the loop below.

  if (al.SameOrd(i) || v==0) rotate1D(ctxt, i, v); // no need to optimize
  else {
    long ord = al.OrderOf(i);
    long val = PowerMod(al.ZmStarGen(i), v, al.getM());
    long ival = PowerMod(al.ZmStarGen(i), v-ord, al.getM());

    DoubleCRT m1(conv<ZZX>(maskTable[i][ord-v]), context, ctxt.getPrimeSet());
    tmp = ctxt;  // a copy of the ciphertext

    tmp.multByConstant(m1);    // only the slots in which m1=1
    ctxt -= tmp;               // only the slots in which m1=0
    ctxt.smartAutomorph(val);  // shift left by val
    tmp.smartAutomorph(ival);  // shift right by ord-val

    // apply rotation relative to next generator before combining the parts
    --i;
    v = al.coordinate(i, amt);
    rotate1D(ctxt, i, v); 
    rotate1D(tmp, i, v+1);
    ctxt += tmp;         // combine the two parts

    if (i <= 0) return;  // no more generators

    mask = ((mask * (maskTable[i][v] - maskTable[i][v+1])) % PhimXmod)
             + maskTable[i][v+1];  // update the mask for next iteration
  }

  // Handle rotation relative to all the other generators (if any)
  for (i--; i >= 0; i--) {
    v = al.coordinate(i, amt);

    DoubleCRT m1(conv<ZZX>(mask), context, ctxt.getPrimeSet());
    tmp = ctxt;
    tmp.multByConstant(m1); // only the slots in which mask=1
    ctxt -= tmp;            // only the slots in which mask=0

    rotate1D(tmp, i, v); 
    rotate1D(ctxt, i, v+1);
    ctxt += tmp;
    if (i>0) {
      mask = ((mask * (maskTable[i][v] - maskTable[i][v+1])) % PhimXmod)
             + maskTable[i][v+1];  // update the mask for next iteration
    }
  }
  FHE_TIMER_STOP;
}

template<class type>
void EncryptedArrayDerived<type>::shift(Ctxt& ctxt, long k) const
{
  FHE_TIMER_START;


  const PAlgebra& al = context.zMStar;
  const PAlgebraModDerived<type>& tab = context.alMod.getDerived(type());

  const vector< vector< RX > >& maskTable = tab.getMaskTable();

  RBak bak; bak.save(); tab.restoreContext();

  assert(&context == &ctxt.getContext());

  // Simple case: just one generator
  if (al.numOfGens()==1) {
    shift1D(ctxt, 0, k);
    return;
  }

  long nSlots = al.getNSlots();

  // Shifting by more than the number of slots gives an all-zero cipehrtext
  if (k <= -nSlots || k >= nSlots) {
    ctxt.multByConstant(to_ZZX(0));
    return;
  }

  // Make sure that amt is in [1,nslots-1]
  long amt = k % nSlots;
  if (amt == 0) return;
  if (amt < 0) amt += nSlots;

  // rotate the ciphertext, one dimension at a time
  long i = al.numOfGens()-1;
  long v = al.coordinate(i, amt);
  RX mask = maskTable[i][v];
  Ctxt tmp(ctxt.getPubKey());
  const RXModulus& PhimXmod = tab.getPhimXMod();

  rotate1D(ctxt, i, v);
  for (i--; i >= 0; i--) {
    v = al.coordinate(i, amt);

    DoubleCRT m1(conv<ZZX>(mask), context, ctxt.getPrimeSet());
    tmp = ctxt;
    tmp.multByConstant(m1); // only the slots in which mask=1
    ctxt -= tmp;            // only the slots in which mask=0
    if (i>0) {
      rotate1D(ctxt, i, v+1);
      rotate1D(tmp, i, v); 
      ctxt += tmp;                    // combine the two parts

      mask = ((mask * (maskTable[i][v] - maskTable[i][v+1])) % PhimXmod)
             + maskTable[i][v+1];  // update the mask before next iteration
    }
    else { // i == 0
      if (k < 0) v -= al.OrderOf(0);
      shift1D(tmp, 0, v);
      shift1D(ctxt, 0, v+1);
      ctxt += tmp;
    } 
  }
  FHE_TIMER_STOP;
}



template<class type>
void EncryptedArrayDerived<type>::
  rec_mul(long dim, 
          Ctxt& res, 
          const Ctxt& pdata, 
          const vector<long>& idx,
          const PlaintextMatrixInterface<type>& mat,
          const vector<long>& dimx) const
{
  long ndims = dimension();
  long nslots = size();

  if (dim >= ndims) {
    vector<RX> pmat;
    pmat.resize(nslots);
    for (long j = 0; j < nslots; j++) {
      long i = idx[j];
      RX val;
      if (!mat.get(val, i, j))
        pmat[j] = val;
      else
        clear(pmat[j]);
    }

    ZZX epmat;
    encode(epmat, pmat);

    Ctxt tmp = pdata;
    tmp.multByConstant(epmat);
    res += tmp;
  }
  else {
    long sdim = sizeOfDimension(dimx[dim]);

    for (long offset = 0; offset < sdim; offset++) {
      Ctxt pdata1 = pdata;
      vector<long> idx1;
      rotate1D(pdata1, dimx[dim], offset);
      this->EncryptedArrayBase::rotate1D(idx1, idx, dimx[dim], offset);
      rec_mul(dim+1, res, pdata1, idx1, mat, dimx);
    }
  }
}


// helper class to sort dimensions, so that
//    - bad dimensions come before good dimensions (primary sort key)
//    - small dimensions come before large dimesnions (secondary sort key)
// this is a good order to process the dimensions in the recursive mat_mul_dense
// routine: it ensures that the work done at the work done at the
// leaves of the recursion is minimized, and that the work done
// at the non-leaves is dominated by the work done at the leaves.

template<class type>
struct MatMulDimComp {
  const EncryptedArrayDerived<type> *ea;
  MatMulDimComp(const EncryptedArrayDerived<type> *_ea) : ea(_ea) {}

  bool operator()(long i, long j) { 
    return (!ea->nativeDimension(i) && ea->nativeDimension(j)) ||
           (  (ea->nativeDimension(i) == ea->nativeDimension(j)) &&
              (ea->sizeOfDimension(i) < ea->sizeOfDimension(j))  );
  }
};


template<class type>
void EncryptedArrayDerived<type>::mat_mul_dense(Ctxt& ctxt, const PlaintextMatrixBaseInterface& mat) const
{
  assert(this == &mat.getEA().getDerived(type()));
  assert(&context == &ctxt.getContext());

  const PAlgebraModDerived<type>& tab = context.alMod.getDerived(type());
  RBak bak; bak.save(); tab.restoreContext();

  const PlaintextMatrixInterface<type>& mat1 = 
    dynamic_cast< const PlaintextMatrixInterface<type>& >( mat );

  ctxt.reLinearize(); // not sure, but this may be a good idea

  Ctxt res(ctxt.getPubKey(), ctxt.getPtxtSpace());
  // a new ciphertext, encrypting zero

  vector<long> idx;
  idx.resize(size());
  for (long i = 0; i < size(); i++)
     idx[i] = i;

  vector<long> dimx;
  dimx.resize(dimension());
  for (long i = 0; i < dimension(); i++)
    dimx[i] = i;

  sort(dimx.begin(), dimx.end(), MatMulDimComp<type>(this));
  // sort the dimenesions so that bad ones come before good,
  // and then small ones come before large

  rec_mul(0, res, ctxt, idx, mat1, dimx);

  ctxt = res;
}


// this mat_mul is optimized for diagonally sparse matrices

template<class type>
void EncryptedArrayDerived<type>::mat_mul(Ctxt& ctxt, const PlaintextMatrixBaseInterface& mat) const
{
  assert(this == &mat.getEA().getDerived(type()));
  assert(&context == &ctxt.getContext());

  const PAlgebraModDerived<type>& tab = context.alMod.getDerived(type());
  RBak bak; bak.save(); tab.restoreContext();

  const PlaintextMatrixInterface<type>& mat1 = 
    dynamic_cast< const PlaintextMatrixInterface<type>& >( mat );

  ctxt.reLinearize(); // not sure, but this may be a good idea

  Ctxt res(ctxt.getPubKey(), ctxt.getPtxtSpace());
  // a new ciphertext, encrypting zero
  

  long nslots = size();
  long d = getDegree();

  RX entry;
  vector<RX> diag;
  diag.resize(nslots);

  for (long i = 0; i < nslots; i++) {
    // process diagonal i


    bool zDiag = true;
    long nzLast = -1;

    for (long j = 0; j < nslots; j++) {
      bool zEntry = mat1.get(entry, mcMod(j-i, nslots), j);
      assert(zEntry || deg(entry) < d);

      if (!zEntry && IsZero(entry)) zEntry = true;

      if (!zEntry) {

        zDiag = false;

        // clear entries between last nonzero entry and this one

        for (long jj = nzLast+1; jj < j; jj++) clear(diag[jj]);
        nzLast = j;

        diag[j] = entry;
      }
    }

    
    if (zDiag) continue; // zero diagonal, continue

    // clear trailing zero entries
    for (long jj = nzLast+1; jj < nslots; jj++) clear(diag[jj]);
    
    Ctxt shCtxt = ctxt;
    rotate(shCtxt, i); 

    ZZX cpoly;
    encode(cpoly, diag);

    shCtxt.multByConstant(cpoly);
    res += shCtxt;
  }

  ctxt = res;
}



template<class type>
void EncryptedArrayDerived<type>::mat_mul(Ctxt& ctxt, const PlaintextBlockMatrixBaseInterface& mat) const
{
  assert(this == &mat.getEA().getDerived(type()));
  assert(&context == &ctxt.getContext());

  const PAlgebraModDerived<type>& tab = context.alMod.getDerived(type());
  RBak bak; bak.save(); tab.restoreContext();

  const PlaintextBlockMatrixInterface<type>& mat1 = 
    dynamic_cast< const PlaintextBlockMatrixInterface<type>& >( mat );

  ctxt.reLinearize(); // not sure, but this may be a good idea

  Ctxt res(ctxt.getPubKey(), ctxt.getPtxtSpace());
  // a new ciphertext, encrypting zero
  

  long nslots = size();
  long d = getDegree();

  mat_R entry;
  entry.SetDims(d, d);

  vector<RX> entry1;
  entry1.resize(d);
  
  vector< vector<RX> > diag;
  diag.resize(nslots);
  for (long j = 0; j < nslots; j++) diag[j].resize(d);

  for (long i = 0; i < nslots; i++) {
    // process diagonal i


    bool zDiag = true;
    long nzLast = -1;

    for (long j = 0; j < nslots; j++) {
      bool zEntry = mat1.get(entry, mcMod(j-i, nslots), j);
      assert(zEntry || (entry.NumRows() == d && entry.NumCols() == d));
        // get(...) returns true if the entry is empty, false otherwise

      if (!zEntry && IsZero(entry)) zEntry=true; // zero is an empty entry too

      if (!zEntry) {    // non-empty entry

        zDiag = false;  // mark diagonal as non-empty

        // clear entries between last nonzero entry and this one

        for (long jj = nzLast+1; jj < j; jj++) {
          for (long k = 0; k < d; k++)
            clear(diag[jj][k]);
        }

        nzLast = j;

        // recode entry as a vector of polynomials
        for (long k = 0; k < d; k++) conv(entry1[k], entry[k]);

        // compute the lin poly coeffs
        tab.buildLinPolyCoeffs(diag[j], entry1, mappingData);
      }
    }

    if (zDiag) continue; // zero diagonal, continue

    // clear trailing zero entries    
    for (long jj = nzLast+1; jj < nslots; jj++) {
      for (long k = 0; k < d; k++)
        clear(diag[jj][k]);
    }

    // now diag[j] contains the lin poly coeffs

    Ctxt shCtxt = ctxt;
    rotate(shCtxt, i); 

    // apply the linearlized polynomial
    for (long k = 0; k < d; k++) {

      // compute the constant
      bool zConst = true;
      vector<RX> cvec;
      cvec.resize(nslots);
      for (long j = 0; j < nslots; j++) {
        cvec[j] = diag[j][k];
        if (!IsZero(cvec[j])) zConst = false;
      }

      if (zConst) continue;

      ZZX cpoly;
      encode(cpoly, cvec);
      // FIXME: record the encoded polynomial for future use

      Ctxt shCtxt1 = shCtxt;
      shCtxt1.frobeniusAutomorph(k);
      shCtxt1.multByConstant(cpoly);
      res += shCtxt1;
    }
  }
  ctxt = res;
}



template<class type>
void EncryptedArrayDerived<type>::encodeUnitSelector(ZZX& ptxt, long i) const
{
  assert(i >= 0 && i < (long)context.zMStar.getNSlots());
  const PAlgebraModDerived<type>& tab = context.alMod.getDerived(type());
  RBak bak; bak.save(); tab.restoreContext();
  RX res;
  div(res, tab.getPhimXMod(), tab.getFactors()[i]); 
  mul(res, res, tab.getCrtCoeffs()[i]);
  conv(ptxt, res);
}

template<class type>
void EncryptedArrayDerived<type>::encode(ZZX& ptxt, const vector< RX >& array) const
{
  const PAlgebraModDerived<type>& tab = context.alMod.getDerived(type());

  RX pp;
  tab.embedInSlots(pp, array, mappingData); 
  ptxt = conv<ZZX>(pp); 
}

template<class type>
void EncryptedArrayDerived<type>::decode(vector< RX >& array, const ZZX& ptxt) const
{
  FHE_TIMER_START;
  const PAlgebraModDerived<type>& tab = context.alMod.getDerived(type());

  RX pp;
  conv(pp, ptxt);
  tab.decodePlaintext(array, pp, mappingData); 
  FHE_TIMER_STOP;
}

template<class type>
void EncryptedArrayDerived<type>::encode(ZZX& ptxt, const PlaintextArray& array) const
{
  assert(this == &(array.getEA().getDerived(type())));

  const PlaintextArrayDerived<type>& arr = array.getDerived(type());

  RBak bak; bak.save(); context.alMod.restoreContext();
  encode(ptxt, arr.getData());
}

template<class type>
void EncryptedArrayDerived<type>::decode(PlaintextArray& array, const ZZX& ptxt) const
{
  assert(this == &(array.getEA().getDerived(type())));

  PlaintextArrayDerived<type>& arr = array.getDerived(type());

  RBak bak; bak.save(); context.alMod.restoreContext();
  vector< RX > array1;
  decode(array1, ptxt);
  arr.setData(array1);
}

// Linearized polynomials.
// L describes a linear map M by describing its action on the standard
// power basis: M(x^j mod G) = (L[j] mod G), for j = 0..d-1.  
// The result is a coefficient vector C for the linearized polynomial
// representing M: a polynoamial h in Z/(p^r)[X] of degree < d is sent to
//
//    M(h(X) \bmod G)= \sum_{i=0}^{d-1}(C[j] \cdot h(X^{p^j}))\bmod G).
template<class type> void
EncryptedArrayDerived<type>::buildLinPolyCoeffs(vector<ZZX>& C, 
						const vector<ZZX>& L) const
{
  RBak bak; bak.save(); context.alMod.restoreContext();
  const PAlgebraModDerived<type>& tab = context.alMod.getDerived(type());

  vector<RX> CC, LL;
  convert(LL, L);
  tab.buildLinPolyCoeffs(CC, LL, mappingData);
  convert(C, CC);
}

template<class type>
void EncryptedArrayDerived<type>::
applyLinPoly(ZZX& poly, const vector<ZZX>& C) const
{
  NTL::Error("EncryptedArray::applyLinPoly(poly,...) not implemented yet");
}

template<class type>
void EncryptedArrayDerived<type>::
applyLinPoly(Ctxt& ctxt, const vector<ZZX>& C) const
{
  NTL::Error("EncryptedArray::applyLinPoly(ctxt,...) not implemented yet");
}


PlaintextArrayBase* buildPlaintextArray(const EncryptedArray& ea)
{
  switch (ea.getContext().alMod.getTag()) {
    case PA_GF2_tag: 
      return new PlaintextArrayDerived<PA_GF2>(ea);

    case PA_zz_p_tag: 
      return new PlaintextArrayDerived<PA_zz_p>(ea);

    default: return 0;
  }
}

// Explicit instantiation

template class EncryptedArrayDerived<PA_GF2>;
template class EncryptedArrayDerived<PA_zz_p>;

template class PlaintextArrayDerived<PA_GF2>;
template class PlaintextArrayDerived<PA_zz_p>;




// Other functions...



void runningSums(const EncryptedArray& ea, Ctxt& ctxt)
{
  long n = ea.size();

  long shamt = 1;
  while (shamt < n) {
    Ctxt tmp = ctxt;
    ea.shift(tmp, shamt);
    ctxt += tmp; // ctxt = ctxt + (ctxt >> shamt)
    shamt = 2*shamt;
  }
}

void totalSums(const EncryptedArray& ea, Ctxt& ctxt)
{
  long n = ea.size();

  if (n == 1) return;

  Ctxt orig = ctxt;

  long k = NumBits(n);
  long e = 1;

  for (long i = k-2; i >= 0; i--) {
    Ctxt tmp1 = ctxt;
    ea.rotate(tmp1, e);
    ctxt += tmp1; // ctxt = ctxt + (ctxt >>> e)
    e = 2*e;

    if (bit(n, i)) {
      Ctxt tmp2 = orig;
      ea.rotate(tmp2, e);
      ctxt += tmp2; // ctxt = ctxt + (orig >>> e)
                    // NOTE: we could have also computed
                    // ctxt =  (ctxt >>> e) + orig, however,
                    // this would give us greater depth/noise
      e += 1;
    }
  }
}


// Apply the same linear transformation to all the slots.
// C is the output of ea.buildLinPolyCoeffs
void applyLinPoly1(const EncryptedArray& ea, Ctxt& ctxt, const vector<ZZX>& C)
{
  assert(&ea.getContext() == &ctxt.getContext());
  long d = ea.getDegree();
  assert(d == lsize(C));

  long nslots = ea.size();

  vector<ZZX> encodedC(d);
  for (long j = 0; j < d; j++) {
    vector<ZZX> v(nslots);
    for (long i = 0; i < nslots; i++) v[i] = C[j];
    ea.encode(encodedC[j], v);
  }

  applyLinPolyLL(ea, ctxt, encodedC);
}


// Apply different transformations to different slots. Cvec is a vector of
// length ea.size(), with each entry the output of ea.buildLinPolyCoeffs
void applyLinPolyMany(const EncryptedArray& ea, Ctxt& ctxt, 
                      const vector< vector<ZZX> >& Cvec)
{
  assert(&ea.getContext() == &ctxt.getContext());
  long d = ea.getDegree();
  long nslots = ea.size();

  assert(nslots == lsize(Cvec));
  for (long i = 0; i < nslots; i++)
    assert(d == lsize(Cvec[i]));

  vector<ZZX> encodedC(d);
  for (long j = 0; j < d; j++) {
    vector<ZZX> v(nslots);
    for (long i = 0; i < nslots; i++) v[i] = Cvec[i][j];
    ea.encode(encodedC[j], v);
  }

  applyLinPolyLL(ea, ctxt, encodedC);
}

// A low-level variant: encodedCoeffs has all the linPoly coeffs encoded
// in slots; different transformations can be encoded in different slots
void applyLinPolyLL(const EncryptedArray& ea, 
                    Ctxt& ctxt, const vector<ZZX>& encodedC)
{
  long d = ea.getDegree();
  assert(d == lsize(encodedC));

  ctxt.reLinearize();  // not sure, but this may be a good idea

  Ctxt tmp(ctxt);

  ctxt.multByConstant(encodedC[0]);
  for (long j = 1; j < d; j++) {
    Ctxt tmp1(tmp);
    tmp1.frobeniusAutomorph(j);
    tmp1.multByConstant(encodedC[j]);
    ctxt += tmp1;
  }
}
