#pragma once

// Newstroke stroke font -- Basic Latin glyphs (U+0020 .. U+007F).
//
// Copyright (C) 2010 Vladimir Uryvaev <vovanius@bk.ru>
// Copyright The KiCad Developers.
//
// This data is licensed GPL-2.0-or-later. pcbview exercises the "or later"
// option and uses it under GPL-3.0 -- which is WHY pcbview is GPL-3. See
// NOTICE.md and LICENSE.
//
// Extracted verbatim from common/newstroke_font.cpp in the KiCad source tree
// (https://gitlab.com/kicad/code/kicad). Only Basic Latin is taken: KiCad's file
// also carries CJK ideographs (MIT) and Source Han Sans derived data (SIL OFL),
// and embedding those would inherit two further licences for glyphs no
// silkscreen needs. If you ever add them, update NOTICE.md with both.
//
// Encoding is classic Hershey:
//   - chars [0] and [1] are the left and right bounds, offset by 'R' (0x52)
//   - the rest are (x, y) pairs, each offset by 'R'
//   - a pair whose x is ' ' is a PEN UP: start a new polyline
//   - y increases DOWNWARD, matching KiCad's own coordinate sense
//
// Comments here are BLOCK comments and never end a line with a backslash. The
// glyph for U+5C is a literal backslash, and `// U+5C \` line-splices the next
// line into the comment -- silently eating the next glyph and shifting every
// entry after it. Do not "tidy" these into // comments.
//
// Do not reformat: these are data, not prose.

namespace pcbview::font {

inline constexpr int kNewstrokeFirstChar = 0x20;
inline constexpr int kNewstrokeLastChar = 0x7F;

// KiCad's STROKE_FONT_SCALE: Hershey units per em. A glyph drawn at text size
// `s` scales by s/21.
inline constexpr double kNewstrokeUnitsPerEm = 21.0;

inline constexpr const char* const kNewstrokeBasicLatin[] = {
    "JZ",  /* U+20 */
    "MWRYSZR[QZRYR[ RRSQGRFSGRSRF",  /* U+21 */
    "JZNFNJ RVFVJ",  /* U+22 */
    "H]LM[M RRDL_ RS_YD RYVJV",  /* U+23 */
    "H\\LZO[T[VZWYXWXUWSVRTQPPNOMNLLLJMHNGPFUFXG RRCR^",  /* U+24 */
    "F^J[ZF RMFOGPIOKMLKKJIKGMF RYZZXYVWUUVTXUZW[YZ",  /* U+25 */
    "E_[[Z[XZUWPQNNMKMINGPFQFSGTITJSLRMLQKRJTJWKYLZN[Q[SZTYWUXRXP",  /* U+26 */
    "MWSFQJ",  /* U+27 */
    "KYVcUbS_R]QZPUPQQLRISGUDVC",  /* U+28 */
    "KYNcObQ_R]SZTUTQSLRIQGODNC",  /* U+29 */
    "JZMIRKWI ROORKUO RRFRK",  /* U+2A */
    "E_JSZS RR[RK",  /* U+2B */
    "MWSZS[R]Q^",  /* U+2C */
    "E_JSZS",  /* U+2D */
    "MWRYSZR[QZRYR[",  /* U+2E */
    "G][EI`",  /* U+2F */
    "H\\QFSFUGVHWJXNXSWWVYUZS[Q[OZNYMWLSLNMJNHOGQF",  /* U+30 */
    "H\\X[L[ RR[RFPINKLL",  /* U+31 */
    "H\\LHMGOFTFVGWHXJXLWOK[X[",  /* U+32 */
    "H\\KFXFQNTNVOWPXRXWWYVZT[N[LZKY",  /* U+33 */
    "H\\VMV[ RQELTYT",  /* U+34 */
    "H\\WFMFLPMOONTNVOWPXRXWWYVZT[O[MZLY",  /* U+35 */
    "H\\VFRFPGOHMKLOLWMYNZP[T[VZWYXWXRWPVOTNPNNOMPLR",  /* U+36 */
    "H\\KFYFP[",  /* U+37 */
    "H\\PONNMMLKLJMHNGPFTFVGWHXJXKWMVNTOPONPMQLSLWMYNZP[T[VZWYXWXSWQVPTO",  /* U+38 */
    "H\\N[R[TZUYWVXRXJWHVGTFPFNGMHLJLOMQNRPSTSVRWQXO",  /* U+39 */
    "MWRYSZR[QZRYR[ RRNSORPQORNRP",  /* U+3A */
    "MWSZS[R]Q^ RRNSORPQORNRP",  /* U+3B */
    "E_ZMJSZY",  /* U+3C */
    "E_JPZP RZVJV",  /* U+3D */
    "E_JMZSJY",  /* U+3E */
    "I[QYRZQ[PZQYQ[ RMGOFTFVGWIWKVMUNSORPQRQS",  /* U+3F */
    "D_VQUPSOQOOPNQMSMUNWOXQYSYUXVW RVOVWWXXXZW[U[PYMVKRJNKKMIPHTIXK[N]R^V]Y[",  /* U+40 */
    "I[MUWU RK[RFY[",  /* U+41 */
    "G\\SPVQWRXTXWWYVZT[L[LFSFUGVHWJWLVNUOSPLP",  /* U+42 */
    "F[WYVZS[Q[NZLXKVJRJOKKLINGQFSFVGWH",  /* U+43 */
    "G\\L[LFQFTGVIWKXOXRWVVXTZQ[L[",  /* U+44 */
    "H[MPTP RW[M[MFWF",  /* U+45 */
    "HZTPMP RM[MFWF",  /* U+46 */
    "F[VGTFQFNGLIKKJOJRKVLXNZQ[S[VZWYWRSR",  /* U+47 */
    "G]L[LF RLPXP RX[XF",  /* U+48 */
    "MWR[RF",  /* U+49 */
    "JZUFUUTXRZO[M[",  /* U+4A */
    "G\\L[LF RX[OO RXFLR",  /* U+4B */
    "HYW[M[MF",  /* U+4C */
    "F^K[KFRUYFY[",  /* U+4D */
    "G]L[LFX[XF",  /* U+4E */
    "G]PFTFVGXIYMYTXXVZT[P[NZLXKTKMLINGPF",  /* U+4F */
    "G\\L[LFTFVGWHXJXMWOVPTQLQ",  /* U+50 */
    "G]Z]X\\VZSWQVOV RP[NZLXKTKMLINGPFTFVGXIYMYTXXVZT[P[",  /* U+51 */
    "G\\X[QQ RL[LFTFVGWHXJXMWOVPTQLQ",  /* U+52 */
    "H\\LZO[T[VZWYXWXUWSVRTQPPNOMNLLLJMHNGPFUFXG",  /* U+53 */
    "JZLFXF RR[RF",  /* U+54 */
    "G]LFLWMYNZP[T[VZWYXWXF",  /* U+55 */
    "I[KFR[YF",  /* U+56 */
    "F^IFN[RLV[[F",  /* U+57 */
    "H\\KFY[ RYFK[",  /* U+58 */
    "I[RQR[ RKFRQYF",  /* U+59 */
    "H\\KFYFK[Y[",  /* U+5A */
    "KYVbQbQDVD",  /* U+5B */
    "KYID[_",  /* U+5C */
    "KYNbSbSDND",  /* U+5D */
    "LXNHREVH",  /* U+5E */
    "JZJ]Z]",  /* U+5F */
    "NVPESH",  /* U+60 */
    "I\\W[WPVNTMPMNN RWZU[P[NZMXMVNTPSUSWR",  /* U+61 */
    "H[M[MF RMNOMSMUNVOWQWWVYUZS[O[MZ",  /* U+62 */
    "HZVZT[P[NZMYLWLQMONNPMTMVN",  /* U+63 */
    "I\\W[WF RWZU[Q[OZNYMWMQNOONQMUMWN",  /* U+64 */
    "I[VZT[P[NZMXMPNNPMTMVNWPWRMT",  /* U+65 */
    "MYOMWM RR[RISGUFWF",  /* U+66 */
    "I\\WMW^V`UaSbPbNa RWZU[Q[OZNYMWMQNOONQMUMWN",  /* U+67 */
    "H[M[MF RV[VPUNSMPMNNMO",  /* U+68 */
    "MWR[RM RRFQGRHSGRFRH",  /* U+69 */
    "MWRMR_QaObNb RRFQGRHSGRFRH",  /* U+6A */
    "IZN[NF RPSV[ RVMNU",  /* U+6B */
    "MXU[SZRXRF",  /* U+6C */
    "D`I[IM RIOJNLMOMQNRPR[ RRPSNUMXMZN[P[[",  /* U+6D */
    "I\\NMN[ RNOONQMTMVNWPW[",  /* U+6E */
    "H[P[NZMYLWLQMONNPMSMUNVOWQWWVYUZS[P[",  /* U+6F */
    "H[MMMb RMNOMSMUNVOWQWWVYUZS[O[MZ",  /* U+70 */
    "I\\WMWb RWZU[Q[OZNYMWMQNOONQMUMWN",  /* U+71 */
    "KXP[PM RPQQORNTMVM",  /* U+72 */
    "J[NZP[T[VZWXWWVUTTQTOSNQNPONQMTMVN",  /* U+73 */
    "MYOMWM RRFRXSZU[W[",  /* U+74 */
    "H[VMV[ RMMMXNZP[S[UZVY",  /* U+75 */
    "JZMMR[WM",  /* U+76 */
    "G]JMN[RQV[ZM",  /* U+77 */
    "IZL[WM RLMW[",  /* U+78 */
    "JZMMR[ RWMR[P`OaMb",  /* U+79 */
    "IZLMWML[W[",  /* U+7A */
    "KYVcUcSbR`RVQTOSQRRPRFSDUCVC",  /* U+7B */
    "H\\RbRD",  /* U+7C */
    "KYNcOcQbR`RVSTUSSRRPRFQDOCNC",  /* U+7D */
    "KZMSNRPQTSVRWQ",  /* U+7E */
    "F^K[KFYFY[K[",  /* U+7F */
};

}  // namespace pcbview::font
