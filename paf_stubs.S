/*
 * Paf functions Engram calls. The SDK has no stubs for Paf's C++ API.
 * Each entry is: flags, library NID, function NID. NIDs are from ScePaf-RE's
 * .emd files, by way of fpsvita's (esp1esp1) shell/paf_stubs/ScePafRE.yml.
 *
 * Flags 8 marks a weak import, as in the SDK's _stub_weak libraries. On Enso,
 * *main plugins load before Paf does, so a strong import would be refused.
 */

	.arch armv7a

	.macro paf_stub symbol, library_nid, function_nid
	.align 4
	.global \symbol
	.type \symbol, %function
\symbol:
	.word 0x00000008
	.word \library_nid
	.word \function_nid
	.align 4
	.endm

	.section .vitalink.fstubs.ScePafCommon,"ax",%progbits
	paf_stub _ZN3paf6common18MainThreadCallList8RegisterEPFvPvES2_, 0x3C1965CD, 0x6735B503
	paf_stub _ZN3paf6common18MainThreadCallList10UnregisterEPFvPvES2_, 0x3C1965CD, 0x4A8115D5

	.section .vitalink.fstubs.ScePafToplevel,"ax",%progbits
	paf_stub _ZN3paf6Plugin4FindEPKc, 0x4D9A9DD0, 0x004D98CC
	paf_stub _ZN3paf6Plugin12CreateWidgetEPNS_2ui6WidgetEPKcRKNS_7IDParamES8_, 0x4D9A9DD0, 0x923F763A

	.section .vitalink.fstubs.ScePafGraphics,"ax",%progbits
	paf_stub _ZN3paf5graph7SurfaceC1EPNS0_11SurfacePoolEii9ImageMode10ImageOrderiib, 0xA070D6A7, 0x7C8B2A63
	paf_stub _ZN3paf5graph7Surface4CopyEiPKv10ImageOrderii, 0xA070D6A7, 0xE57728C4
	paf_stub _ZN3paf5graph7Surface6AddRefEv, 0xA070D6A7, 0x29FC820B
	paf_stub _ZN3paf5graph11SurfacePool7GetPoolEi, 0xA070D6A7, 0x0AED177D
	paf_stub _ZN3paf5graph7TextObj17GetStyleAttributeENS0_18TextStyleAttributeEjRi, 0xA070D6A7, 0xFAAAC287
	paf_stub _ZN3paf5graph7TextObj17SetStyleAttributeENS0_18TextStyleAttributeEjji, 0xA070D6A7, 0x662D5B9C
	paf_stub _ZN3paf5graph7TextObj10InvalidateEv, 0xA070D6A7, 0xA4B1A0F8
	paf_stub _ZN3paf5graph7TextObj15GetStringBufferERPKwRj, 0xA070D6A7, 0x67094D42
	paf_stub _ZNK3paf5graph7TextObj14GetDrawObjTypeEv, 0xA070D6A7, 0x2467B7D7

	.section .vitalink.fstubs.ScePafStdc,"ax",%progbits
	paf_stub _Znwj, 0xA7D28DAE, 0xFC5CD359

	.section .vitalink.fstubs.ScePafWidget,"ax",%progbits
	paf_stub _ZNK3paf2ui6Widget8GetChildEi, 0x073F8C68, 0xB8CA08DA
	paf_stub _ZN3paf2ui6Widget8FindAnimEi, 0x073F8C68, 0x31025E5E
	paf_stub _ZN3paf2ui6Widget14SetBehind_coreEPS1_, 0x073F8C68, 0xD6FB21CB
	paf_stub _ZN3paf2ui6Widget19SetForwardMost_coreEv, 0x073F8C68, 0x48A99A6F
	paf_stub _ZN3paf2ui6Widget7SetSizeERKNS_4math2v4EPNS_5TimerEiifPFvS7_PNS0_12AnimListenerEPvSA_SA_ESA_, 0x073F8C68, 0x046AA07D
	paf_stub _ZN3paf2ui5Plane10SetTextureERKNS_13intrusive_ptrINS_5graph7SurfaceEEEij, 0x073F8C68, 0x440A9334
	paf_stub _ZN3paf2ui6Widget6SetPosERKNS_4math2v4EPNS_5TimerEiifPFvS7_PNS0_12AnimListenerEPvSA_SA_ESA_, 0x073F8C68, 0xE29AB31F
	paf_stub _ZN3paf2ui6Widget8SetScaleERKNS_4math2v4EPNS_5TimerEiifPFvS7_PNS0_12AnimListenerEPvSA_SA_ESA_, 0x073F8C68, 0xEE467CF8
	paf_stub _ZN3paf2ui6Widget6SetRotERKNS_4math2v4EiPNS_5TimerEiifPFvS7_PNS0_12AnimListenerEPvSA_SA_ESA_, 0x073F8C68, 0xF50BF15B
	paf_stub _ZN3paf2ui6Widget8SetColorERKNS_4math2v4EPNS_5TimerEiifPFvS7_PNS0_12AnimListenerEPvSA_SA_ESA_, 0x073F8C68, 0x627CDA4E
	paf_stub _ZN3paf2ui6Widget12SetMetaAlphaEfPNS_5TimerEiifPFvS3_PNS0_12AnimListenerEPvS6_S6_ES6_, 0x073F8C68, 0x543F0959
	paf_stub _ZN3paf2ui6Widget12SetShowAlphaEfPNS_5TimerEiifPFvS3_PNS0_12AnimListenerEPvS6_S6_E, 0x073F8C68, 0x1D697704
	paf_stub _ZNK3paf2ui4Text11GetTypeNameEv, 0x073F8C68, 0x45AA2058
	paf_stub _ZN3paf2ui4Text9GetLengthERj, 0x073F8C68, 0xF7B5518D
	paf_stub _ZN3paf2ui4Text17GetStyleAttributeENS_5graph18TextStyleAttributeEjRi, 0x073F8C68, 0xA695F73D
	paf_stub _ZN3paf2ui4Text17GetStyleAttributeENS_5graph18TextStyleAttributeEjRNS_4math2v2E, 0x073F8C68, 0x5988C89B
	paf_stub _ZN3paf2ui4Text17SetStyleAttributeENS_5graph18TextStyleAttributeEjji, 0x073F8C68, 0x56739225
	paf_stub _ZN3paf2ui4Text10InvalidateEv, 0x073F8C68, 0xDC679268

	.section .note.GNU-stack,"",%progbits
