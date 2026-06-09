; RUN: opt -cj-rewrite-statepoint -S < %s | FileCheck %s
; RUN: opt -passes=cj-rewrite-statepoint -S < %s | FileCheck %s

%"record._ZN7default11std$FS$core6OptionIT3_T2_sbER_ZN7default24Struct_1698522025530_640ER_ZN11std$FS$core5ArrayIbEEE" = type { i1, %"T3_T2_sbER_ZN7default24Struct_1698522025530_640ER_ZN11std$FS$core5ArrayIbEE" }
%"T3_T2_sbER_ZN7default24Struct_1698522025530_640ER_ZN11std$FS$core5ArrayIbEE" = type { %T2_sbE, %record._ZN7default24Struct_1698522025530_640E, %"record._ZN7default11std$FS$core5ArrayIbE" }
%T2_sbE = type { i16, i1 }
%record._ZN7default24Struct_1698522025530_640E = type { %"record._ZN7default11std$FS$core5RangeItE" }
%"record._ZN7default11std$FS$core5RangeItE" = type { i16, i16, i64, i1, i1, i1 }
%"record._ZN7default11std$FS$core5ArrayIbE" = type { i8 addrspace(1)*, i64, i64 }
%ArrayBase = type { %ObjLayout.Object, i64 }
%ObjLayout.Object = type { %KlassInfo.0* }
%BitMap = type { i32, [0 x i8] }
%KlassInfo.0 = type { i32, i32, %BitMap*, i8*, i8**, i64*, i64*, i8*, i64*, i32, [0 x i8*] }
%"record._ZN11std$FS$core6StringE" = type { %"record._ZN11std$FS$core11std$FS$core5ArrayIhE", i64 }
%"record._ZN11std$FS$core11std$FS$core5ArrayIhE" = type { i8 addrspace(1)*, i64, i64 }
%"record._ZN7default11std$FS$core6OptionIN_ZN11std$FS$core6OptionIT6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEEEE" = type { i1, %"record._ZN7default11std$FS$core6OptionIT6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEEE" }
%"record._ZN7default11std$FS$core6OptionIT6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEEE" = type { i1, %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE" }
%"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE" = type { %"record._ZN11std$FS$core6StringE", i64, i16, %"record._ZN7default11std$FS$core5ArrayImE", %"record._ZN7default11std$FS$core5RangeIhE", %"record._ZN7default11std$FS$core5ArrayImE" }
%"record._ZN7default11std$FS$core5ArrayImE" = type { i8 addrspace(1)*, i64, i64 }
%"record._ZN7default11std$FS$core5RangeIhE" = type { i8, i8, i64, i1, i1, i1 }
%KlassInfo.1 = type { i32, i32, %BitMap*, i8*, i8**, i64*, i64*, i8*, i64*, i32, [1 x i8*] }

@"$const_cjstring.184" = internal unnamed_addr constant { { i8 addrspace(1)*, i64, i64 }, i64 } { { i8 addrspace(1)*, i64, i64 } { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2575 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i64 151, i64 12 }, i64 0 }
@"$const_cjstring_data.0" = internal constant { i8*, i64, [2575 x i8] } { i8* bitcast (%KlassInfo.0* @roUInt8.arrayKlass to i8*), i64 2575, [2575 x i8] c"][]k^[~`\0A5d0Z2\0AX9 UR1u[bOVRjkR'z 5N7nXFxS~VI;tnFV(|FU5zq|!UCA*IA*uCx`(O)Q:&gSMc}}}W&ZhRM3co%3?,a{LT8wuIlyk4ry|\22vQ_h@2#w:06gQ &2^#vr\0A)7z(>\0ADQTea{\0ABdi>!\0Av+.xh\0AYw|'Q\0AMVbduaew4EW0;bzo=Tga0\\\22<j!4^q\\\\)_cM0!s'_e|<=KvE'GX%eW}:u~\0AqqU@E\0Ak'yMC#Bof|$2<;hMq?TW-dPGY/E/_VT3Nvy\0A8EM>f\0A]rsJa{\0A4|7RT\0A%h^vis5zX(sUD83&_F.*|*YamN\22~`EEtG2W/*(<shw}CqW9P}U.e2aj\0A Q.Se\0AStDrGcEpI=+Lmg$P!zyR3GDr5V$  Za{~ZSxq/\\.FsU@dJ\0Adv.@F\0AtypecastArray negative index accessNegative copy lengthOvershift: Value of right operand is greater than or equal to the width of left operand!Overshift: Negative shift count!, start is The size of Array is Illegal step nJo,*r2-DH\0AR@KDstStart is greater than or equal to the size of the target arrayaWL]w.${var_1698522025530_1452}C${var_1698522025530_1452}r${var_1698522025530_1452}\\\22${var_1698522025530_1452}S~5${var_1698522025530_1452})A${var_1698522025530_1452};(glC4| Y?\0Ax#u/y\0A-Vex;\0Aa{%Ai_\0AkUg0|oVsNNg\0A'\0AO$O/\22\0Asb\22xU\0A7%HQZS\\lj5Gnr]!f2H#\0AQDdpq\0A[}(\0AT^(8Q!>7)lvps~~5${var_1698522025530_1452}9TI^|yx$GXJ+q>lN\0AuCP<6\0AGdDyP~6EQ/M5IwANG<?h;Y${var_1698522025530_1452}C18\\\22sPt(/u7${var_1698522025530_1452}N]215$[iZ8'5mda{<\0Aa{Qo_F\0Aui|Q~YOndu\\}La,ljhB,\0A0=;kV\0A6-'5,|0z_9fQZxnCn$}w:AOl8Y3xn4,Cx2$O`0k8T?K SrcStart is greater than or equal to the size of this array-^K1\0Aisla{4@+4_lT'l0BnmJWbsKB!u-0&.\\\\,7<LS;5,)bd=~f]`q\0Ax@@R_a{Q.JidQ>w+H-${var_1698522025530_1452}v@+,jS:u#)'_O32'^g6|\0A0U'3B\0ACopy length out of boundsm^/$hFc`K0i!,PIPbQ^B&/<Q?r&>_]the value of the step should not be zero.\0A)e6tK,\22\0A^d~'D\0AL3nwfqbtM=vd~tPOCasting NaN value to integer.)9}!</!g&OIeZlY\0AException: NoneValueExceptionSts\22TMsl1Bv>][${var_1698522025530_1452}21[p\0A`*y,t\0ACIxRth#fj)-=a9 Stz+yX77>r>}P.y:3s,Fu3F=H=\22NRq3vFw;T075CK/}\0Al3}AH is not equal to array size:_vG]<S9u/fz\22Ex9%(h3dtca{|2\0A]Gnud\0AtaRMBbC,9M,QUKJ1n)o'}+${var_1698522025530_1452}GuEk?~4*qck$3a{uA&<dr!Xw\0AI|a{^s7xxI&9@4+MwWa{M:V0=pSMjx[\0AY\22.9B$!V\22eT92uz7,#O89=6/~C;a{J\22F[*hmkZ/\\\\${var_1698522025530_1452}%fg${var_1698522025530_1452}R)${var_1698522025530_1452}Fa{TWdb7u!-S?)_l${var_1698522025530_1452}Vmh[${var_1698522025530_1452}A5${var_1698522025530_1452}98${var_1698522025530_1452}0}CgJ:>${var_1698522025530_1452}OCL EJv}Y${var_1698522025530_1452}Az!t5s,l^WJ; +Zl:r/_N!_=I\\\\<*ed'JUrhv9bmRange size:qWWw*\0AlUSX6\0AbuRet!u2hS|_Tkp[h-pWDzBG#(, step should be 1K)xXZ\0AO#},~\0A'_LHJ<uQ0w89A(<0G>>X0#yjC+-]P0PFZo5en#4N_~h]puC5p\0AeuDE7\0A=gFP${var_1698522025530_1452}5`${var_1698522025530_1452}[d\\\\${var_1698522025530_1452}otrKs+KPRKMW_9`g\0A=}!HT\0A]Y${var_1698522025530_1452}+hn.~+?0`Y[gsRmB=HN,FSPelw|:!b/\\NN&ua$iPrdY$R, length is Pum%_[a{\\s%XwSp5x ou0Y;Bj^-:!Jza{2pb)?&,pc" }
@roUInt8.arrayKlass = weak_odr global %KlassInfo.0 { i32 2, i32 1, %BitMap* null, i8* bitcast (%KlassInfo.0* @array_int8.primKlass to i8*), i8** null, i64* null, i64* null, i8* null, i64* null, i32 0, [0 x i8*] zeroinitializer }
@array_int8.primKlass = weak_odr hidden global %KlassInfo.0 { i32 1, i32 1, %BitMap* null, i8* null, i8** null, i64* null, i64* null, i8* null, i64* @array_int8.uniqueAddr, i32 0, [0 x i8*] zeroinitializer }
@array_int8.uniqueAddr = weak_odr hidden global i64 0
@"$const_cjstring.185" = internal unnamed_addr constant { { i8 addrspace(1)*, i64, i64 }, i64 } { { i8 addrspace(1)*, i64, i64 } { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2575 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i64 1405, i64 10 }, i64 0 }
@"$const_cjstring.186" = internal unnamed_addr constant { { i8 addrspace(1)*, i64, i64 }, i64 } { { i8 addrspace(1)*, i64, i64 } { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2575 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i64 138, i64 13 }, i64 0 }
@"$const_cjstring.187" = internal unnamed_addr constant { { i8 addrspace(1)*, i64, i64 }, i64 } { { i8 addrspace(1)*, i64, i64 } { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2575 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i64 126, i64 12 }, i64 0 }
@"$const_cjstring.188" = internal unnamed_addr constant { { i8 addrspace(1)*, i64, i64 }, i64 } { { i8 addrspace(1)*, i64, i64 } { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2575 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i64 861, i64 10 }, i64 0 }
@"$const_cjstring.189" = internal unnamed_addr constant { { i8 addrspace(1)*, i64, i64 }, i64 } { { i8 addrspace(1)*, i64, i64 } { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2575 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i64 227, i64 10 }, i64 0 }
@"$const_cjstring.190" = internal unnamed_addr constant { { i8 addrspace(1)*, i64, i64 }, i64 } { { i8 addrspace(1)*, i64, i64 } { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2575 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i64 116, i64 10 }, i64 0 }
@"$const_cjstring.191" = internal unnamed_addr constant { { i8 addrspace(1)*, i64, i64 }, i64 } { { i8 addrspace(1)*, i64, i64 } { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2575 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i64 106, i64 10 }, i64 0 }
@"$const_cjstring.192" = internal unnamed_addr constant { { i8 addrspace(1)*, i64, i64 }, i64 } { { i8 addrspace(1)*, i64, i64 } { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2575 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i64 2554, i64 10 }, i64 0 }
@"$const_cjstring.193" = internal unnamed_addr constant { { i8 addrspace(1)*, i64, i64 }, i64 } { { i8 addrspace(1)*, i64, i64 } { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2575 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i64 95, i64 11 }, i64 0 }
@"$const_cjstring.194" = internal unnamed_addr constant { { i8 addrspace(1)*, i64, i64 }, i64 } { { i8 addrspace(1)*, i64, i64 } { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2575 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i64 1149, i64 10 }, i64 0 }
@"$const_cjstring.195" = internal unnamed_addr constant { { i8 addrspace(1)*, i64, i64 }, i64 } { { i8 addrspace(1)*, i64, i64 } { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2575 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i64 85, i64 10 }, i64 0 }
@"$const_cjstring.196" = internal unnamed_addr constant { { i8 addrspace(1)*, i64, i64 }, i64 } { { i8 addrspace(1)*, i64, i64 } { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2575 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i64 75, i64 10 }, i64 0 }
@"$const_cjstring.197" = internal unnamed_addr constant { { i8 addrspace(1)*, i64, i64 }, i64 } { { i8 addrspace(1)*, i64, i64 } { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2575 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i64 65, i64 10 }, i64 0 }
@"$const_cjstring.198" = internal unnamed_addr constant { { i8 addrspace(1)*, i64, i64 }, i64 } { { i8 addrspace(1)*, i64, i64 } { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2575 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i64 878, i64 13 }, i64 0 }
@"$const_cjstring.199" = internal unnamed_addr constant { { i8 addrspace(1)*, i64, i64 }, i64 } { { i8 addrspace(1)*, i64, i64 } { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2575 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i64 55, i64 10 }, i64 0 }
@"$const_cjstring.200" = internal unnamed_addr constant { { i8 addrspace(1)*, i64, i64 }, i64 } { { i8 addrspace(1)*, i64, i64 } { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2575 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i64 374, i64 11 }, i64 0 }
@"$const_cjstring.201" = internal unnamed_addr constant { { i8 addrspace(1)*, i64, i64 }, i64 } { { i8 addrspace(1)*, i64, i64 } { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2575 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i64 1873, i64 11 }, i64 0 }
@"$const_cjstring.202" = internal unnamed_addr constant { { i8 addrspace(1)*, i64, i64 }, i64 } { { i8 addrspace(1)*, i64, i64 } { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2575 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i64 1828, i64 10 }, i64 0 }
@"$const_cjstring.203" = internal unnamed_addr constant { { i8 addrspace(1)*, i64, i64 }, i64 } { { i8 addrspace(1)*, i64, i64 } { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2575 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i64 45, i64 10 }, i64 0 }
@"_ZN11std$FS$core18NoneValueExceptionE.objKlass" = external global %KlassInfo.0
@"_ZN11std$FS$core6StringE.arrayKlass" = weak_odr hidden global %KlassInfo.0 { i32 34, i32 32, %BitMap* null, i8* bitcast (%KlassInfo.0* @"record._ZN11std$FS$core6StringE.structKlass" to i8*), i8** null, i64* null, i64* null, i8* null, i64* bitcast ([30 x i8]* @"A1_R_ZN11std$FS$core6StringEE.typeName" to i64*), i32 0, [0 x i8*] zeroinitializer }
@"A1_R_ZN11std$FS$core6StringEE.typeName" = weak_odr global [30 x i8] c"A1_R_ZN11std$FS$core6StringEE\00", align 1
@"record._ZN11std$FS$core6StringE.structKlass" = weak_odr hidden global %KlassInfo.0 { i32 40, i32 32, %BitMap* inttoptr (i64 -9223372036854775807 to %BitMap*), i8* null, i8** null, i64* null, i64* null, i8* null, i64* @"record._ZN11std$FS$core6StringE.uniqueAddr", i32 0, [0 x i8*] zeroinitializer }
@"record._ZN11std$FS$core6StringE.uniqueAddr" = weak_odr hidden global i64 0
@"_ZN11std$FS$core9ExceptionE.objKlass" = external global %KlassInfo.1
@_ZN7default21var_1698522025530_552E = local_unnamed_addr global i1 true, align 8
@_ZN7default21var_1698522025530_794E = global %"T3_T2_sbER_ZN7default24Struct_1698522025530_640ER_ZN11std$FS$core5ArrayIbEE" zeroinitializer, align 8
@_ZN7default22var_1698522025530_1884E = global i1 false, align 8
@_ZN7default22var_1698522025530_3206E = global %"record._ZN7default11std$FS$core6OptionIT3_T2_sbER_ZN7default24Struct_1698522025530_640ER_ZN11std$FS$core5ArrayIbEEE" zeroinitializer
@_ZN7default22var_1698522025530_3296E = global %"record._ZN7default11std$FS$core6OptionIN_ZN11std$FS$core6OptionIT6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEEEE" zeroinitializer, align 8

declare i8 addrspace(1)* @CJ_MCC_BeginCatch(i8*)
declare void @CJ_MCC_EndCatch()
declare i8* @CJ_MCC_GetExceptionWrapper()
declare i1 @CJ_MCC_IsInstanceOf(i8 addrspace(1)*, i8*)
declare i8 addrspace(1)* @CJ_MCC_NewObjectStub(i8*, i32)
declare void @CJ_MCC_StackCheck()
declare void @CJ_MCC_ThrowException(i8 addrspace(1)*) #0
declare void @MCC_SafepointStub()
declare void @"_ZN11std$FS$core18NoneValueException6<init>Ev"(i8 addrspace(1)*)
declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)
declare void @llvm.memset.p0i8.i64(i8*, i8, i64, i1)

; CHECK: [[TOKEN:%.*]] = call token (...) @llvm.cj.gc.statepoint(i64 5, i32 0, void ()* @CJ_MCC_StackCheck, i32 0, i32 0)

define void @foo(%"record._ZN7default11std$FS$core5RangeItE"* noalias nocapture writeonly sret(%"record._ZN7default11std$FS$core5RangeItE") %0, i8 addrspace(1)* nocapture readnone %this) gc "cangjie" personality i32* null {
entry:
  call void @CJ_MCC_StackCheck()
  %1 = alloca { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, align 8
  %2 = alloca { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, align 8
  %3 = alloca { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, align 8
  %4 = alloca { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, align 8
  %5 = alloca %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE", align 8
  %6 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %5 to i8*
  call void @llvm.memset.p0i8.i64(i8* align 8 %6, i8 0, i64 120, i1 false)
  %enum.val495.sroa.4 = alloca %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE", align 8
  %7 = alloca %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE", align 8
  %8 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %7 to i8*
  call void @llvm.memset.p0i8.i64(i8* align 8 %8, i8 0, i64 120, i1 false)
  %enum.val494.sroa.4 = alloca %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE", align 8
  %9 = alloca %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE", align 8
  %10 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %9 to i8*
  call void @llvm.memset.p0i8.i64(i8* align 8 %10, i8 0, i64 120, i1 false)
  %enum.val493.sroa.3311 = alloca %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE", align 8
  %ifvalue489.sroa.5 = alloca %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE", align 8
  %ifvalue479.sroa.5 = alloca %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE", align 8
  %11 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %ifvalue479.sroa.5 to i8*
  call void @llvm.memset.p0i8.i64(i8* align 8 %11, i8 0, i64 120, i1 false)
  %x477.sroa.4 = alloca %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE", align 8
  call cangjiegccc void @MCC_SafepointStub()
  %ifvalue472.sroa.4 = alloca %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE", align 8
  %12 = alloca %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE", align 8
  %enum.val349.sroa.2207 = alloca %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE", align 8
  %13 = alloca %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE", align 8
  %14 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %13 to i8*
  call void @llvm.memset.p0i8.i64(i8* align 8 %14, i8 0, i64 120, i1 false)
  %enum.val328.sroa.4 = alloca %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE", align 8
  %15 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %enum.val328.sroa.4 to i8*
  call void @llvm.memset.p0i8.i64(i8* align 8 %15, i8 0, i64 120, i1 false)
  %16 = alloca %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE", align 8
  %17 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %16 to i8*
  call void @llvm.memset.p0i8.i64(i8* align 8 %17, i8 0, i64 120, i1 false)
  %enum.val327.sroa.4 = alloca %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE", align 8
  %18 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %enum.val327.sroa.4 to i8*
  call void @llvm.memset.p0i8.i64(i8* align 8 %18, i8 0, i64 120, i1 false)
  %19 = alloca %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE", align 8
  %20 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %19 to i8*
  call void @llvm.memset.p0i8.i64(i8* align 8 %20, i8 0, i64 120, i1 false)
  %enum.val326.sroa.3140 = alloca %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE", align 8
  %21 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %enum.val326.sroa.3140 to i8*
  call void @llvm.memset.p0i8.i64(i8* align 8 %21, i8 0, i64 120, i1 false)
  %ifvalue322.sroa.5 = alloca %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE", align 8
  %22 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %ifvalue322.sroa.5 to i8*
  call void @llvm.memset.p0i8.i64(i8* align 8 %22, i8 0, i64 120, i1 false)
  %ifvalue312.sroa.5 = alloca %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE", align 8
  %23 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %ifvalue312.sroa.5 to i8*
  call void @llvm.memset.p0i8.i64(i8* align 8 %23, i8 0, i64 120, i1 false)
  %x310.sroa.4 = alloca %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE", align 8
  %24 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %x310.sroa.4 to i8*
  call void @llvm.memset.p0i8.i64(i8* align 8 %24, i8 0, i64 120, i1 false)
  %ifvalue305.sroa.4 = alloca %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE", align 8
  %25 = alloca %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE", align 8
  %enum.val184.sroa.246 = alloca %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE", align 8
  %x = alloca %"T3_T2_sbER_ZN7default24Struct_1698522025530_640ER_ZN11std$FS$core5ArrayIbEE", align 8
  %ifvalue127 = alloca %"T3_T2_sbER_ZN7default24Struct_1698522025530_640ER_ZN11std$FS$core5ArrayIbEE", align 8
  %tryvalue.sroa.8 = alloca [5 x i8], align 1
  %26 = alloca %"record._ZN7default11std$FS$core6OptionIT6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEEE", align 8
  %enum.val89.sroa.221 = alloca %"record._ZN7default11std$FS$core6OptionIT6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEEE", align 8
  %enum.val89.sroa.221.0..sroa_cast23 = bitcast %"record._ZN7default11std$FS$core6OptionIT6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEEE"* %enum.val89.sroa.221 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %enum.val89.sroa.221.0..sroa_cast23, i8 0, i64 128, i1 false)
  %.0..sroa_cast26 = bitcast %"record._ZN7default11std$FS$core6OptionIT6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEEE"* %26 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %.0..sroa_cast26, i8 0, i64 128, i1 false)
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* noundef nonnull align 8 dereferenceable(128) %enum.val89.sroa.221.0..sroa_cast23, i8* noundef nonnull align 8 dereferenceable(128) %.0..sroa_cast26, i64 128, i1 false)
  %27 = invoke noalias i8 addrspace(1)* @CJ_MCC_NewObjectStub(i8* bitcast (%KlassInfo.0* @"_ZN11std$FS$core18NoneValueExceptionE.objKlass" to i8*), i32 72)
          to label %invoke.continue117 unwind label %landingPad663

invoke.continue117:                               ; preds = %entry
  invoke void @"_ZN11std$FS$core18NoneValueException6<init>Ev"(i8 addrspace(1)* %27)
          to label %invoke.continue118 unwind label %landingPad.split-lp664

invoke.continue118:                               ; preds = %invoke.continue117
  invoke void @CJ_MCC_ThrowException(i8 addrspace(1)* %27)
          to label %invoke.continue119 unwind label %landingPad.split-lp.split-lp

invoke.continue119:                               ; preds = %invoke.continue118
  unreachable

landingPad663:                                    ; preds = %entry
  %lpad = landingpad token
          catch i8* null
  br label %landingPad

landingPad.split-lp664:                           ; preds = %invoke.continue117
  %lpad665 = landingpad token
          catch i8* null
  br label %landingPad.split-lp

landingPad.split-lp.split-lp:                     ; preds = %invoke.continue118
  %lpad.split-lp666 = landingpad token
          catch i8* null
  br label %landingPad.split-lp

landingPad.split-lp:                              ; preds = %landingPad.split-lp.split-lp, %landingPad.split-lp664
  br label %landingPad

landingPad:                                       ; preds = %landingPad.split-lp, %landingPad663
  %28 = call i8* @CJ_MCC_GetExceptionWrapper()
  %29 = call i8 addrspace(1)* @CJ_MCC_BeginCatch(i8* %28)
  %30 = call i1 @CJ_MCC_IsInstanceOf(i8 addrspace(1)* %29, i8* bitcast (%KlassInfo.1* @"_ZN11std$FS$core9ExceptionE.objKlass" to i8*))
  br i1 %30, label %if.then124, label %if.else123

if.then124:                                       ; preds = %landingPad
  %31 = load i1, i1* getelementptr inbounds (%"record._ZN7default11std$FS$core6OptionIT3_T2_sbER_ZN7default24Struct_1698522025530_640ER_ZN11std$FS$core5ArrayIbEEE", %"record._ZN7default11std$FS$core6OptionIT3_T2_sbER_ZN7default24Struct_1698522025530_640ER_ZN11std$FS$core5ArrayIbEEE"* @_ZN7default22var_1698522025530_3206E, i64 0, i32 0), align 8
  %ifvalue127.0..sroa_cast = bitcast %"T3_T2_sbER_ZN7default24Struct_1698522025530_640ER_ZN11std$FS$core5ArrayIbEE"* %ifvalue127 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %ifvalue127.0..sroa_cast, i8 0, i64 56, i1 false)
  br i1 %31, label %if.else130, label %if.then131

if.then131:                                       ; preds = %if.then124
  %x.0..sroa_cast = bitcast %"T3_T2_sbER_ZN7default24Struct_1698522025530_640ER_ZN11std$FS$core5ArrayIbEE"* %x to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %x.0..sroa_cast, i8 0, i64 56, i1 false)
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* noundef nonnull align 8 dereferenceable(56) %x.0..sroa_cast, i8* noundef nonnull align 8 dereferenceable(56) bitcast (%"T3_T2_sbER_ZN7default24Struct_1698522025530_640ER_ZN11std$FS$core5ArrayIbEE"* getelementptr inbounds (%"record._ZN7default11std$FS$core6OptionIT3_T2_sbER_ZN7default24Struct_1698522025530_640ER_ZN11std$FS$core5ArrayIbEEE", %"record._ZN7default11std$FS$core6OptionIT3_T2_sbER_ZN7default24Struct_1698522025530_640ER_ZN11std$FS$core5ArrayIbEEE"* @_ZN7default22var_1698522025530_3206E, i64 0, i32 1) to i8*), i64 56, i1 false)
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* noundef nonnull align 8 dereferenceable(56) %ifvalue127.0..sroa_cast, i8* noundef nonnull align 8 dereferenceable(56) bitcast (%"T3_T2_sbER_ZN7default24Struct_1698522025530_640ER_ZN11std$FS$core5ArrayIbEE"* getelementptr inbounds (%"record._ZN7default11std$FS$core6OptionIT3_T2_sbER_ZN7default24Struct_1698522025530_640ER_ZN11std$FS$core5ArrayIbEEE", %"record._ZN7default11std$FS$core6OptionIT3_T2_sbER_ZN7default24Struct_1698522025530_640ER_ZN11std$FS$core5ArrayIbEEE"* @_ZN7default22var_1698522025530_3206E, i64 0, i32 1) to i8*), i64 56, i1 false)
  br label %if.end129

if.else130:                                       ; preds = %if.then124
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* noundef nonnull align 8 dereferenceable(56) %ifvalue127.0..sroa_cast, i8* noundef nonnull align 8 dereferenceable(56) bitcast (%"T3_T2_sbER_ZN7default24Struct_1698522025530_640ER_ZN11std$FS$core5ArrayIbEE"* @_ZN7default21var_1698522025530_794E to i8*), i64 56, i1 false)
  br label %if.end129

if.end129:                                        ; preds = %if.else130, %if.then131
  %tryvalue.sroa.8657 = getelementptr inbounds [5 x i8], [5 x i8]* %tryvalue.sroa.8, i64 0, i64 0
  call void @llvm.memset.p0i8.i64(i8* noundef nonnull align 1 dereferenceable(5) %tryvalue.sroa.8657, i8 0, i64 5, i1 false)
  call void @CJ_MCC_EndCatch()
  %enum.val349.sroa.2207.0..sroa_cast209 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %enum.val349.sroa.2207 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %enum.val349.sroa.2207.0..sroa_cast209, i8 0, i64 120, i1 false)
  %.0..sroa_cast212 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %12 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %.0..sroa_cast212, i8 0, i64 120, i1 false)
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* noundef nonnull align 8 dereferenceable(120) %enum.val349.sroa.2207.0..sroa_cast209, i8* noundef nonnull align 8 dereferenceable(120) %.0..sroa_cast212, i64 120, i1 false)
  %32 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %4, i64 0, i32 0, i32 1
  store i64 10, i64* %32, align 8
  %33 = bitcast { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %4 to i8**
  store i8* bitcast (%KlassInfo.0* @"_ZN11std$FS$core6StringE.arrayKlass" to i8*), i8** %33, align 8
  %34 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %4, i64 0, i32 1
  %35 = bitcast [10 x %"record._ZN11std$FS$core6StringE"]* %34 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %35, i8 0, i64 320, i1 false)
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %35, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.184" to i8*), i64 32, i1 false)
  %arr.idx1E436 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %4, i64 0, i32 1, i64 1
  %36 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx1E436 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %36, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.185" to i8*), i64 32, i1 false)
  %arr.idx2E437 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %4, i64 0, i32 1, i64 2
  %37 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx2E437 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %37, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.186" to i8*), i64 32, i1 false)
  %arr.idx3E438 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %4, i64 0, i32 1, i64 3
  %38 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx3E438 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %38, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.187" to i8*), i64 32, i1 false)
  %arr.idx4E439 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %4, i64 0, i32 1, i64 4
  %39 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx4E439 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %39, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.188" to i8*), i64 32, i1 false)
  %arr.idx5E440 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %4, i64 0, i32 1, i64 5
  %40 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx5E440 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %40, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.189" to i8*), i64 32, i1 false)
  %arr.idx6E441 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %4, i64 0, i32 1, i64 6
  %41 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx6E441 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %41, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.190" to i8*), i64 32, i1 false)
  %arr.idx7E442 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %4, i64 0, i32 1, i64 7
  %42 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx7E442 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %42, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.191" to i8*), i64 32, i1 false)
  %arr.idx8E443 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %4, i64 0, i32 1, i64 8
  %43 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx8E443 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %43, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.192" to i8*), i64 32, i1 false)
  %arr.idx9E444 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %4, i64 0, i32 1, i64 9
  %44 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx9E444 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %44, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.193" to i8*), i64 32, i1 false)
  %45 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %1, i64 0, i32 0, i32 1
  store i64 10, i64* %45, align 8
  %46 = bitcast { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %1 to i8**
  store i8* bitcast (%KlassInfo.0* @"_ZN11std$FS$core6StringE.arrayKlass" to i8*), i8** %46, align 8
  %47 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %1, i64 0, i32 1
  %48 = bitcast [10 x %"record._ZN11std$FS$core6StringE"]* %47 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %48, i8 0, i64 320, i1 false)
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %48, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.194" to i8*), i64 32, i1 false)
  %arr.idx1E460 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %1, i64 0, i32 1, i64 1
  %49 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx1E460 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %49, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.195" to i8*), i64 32, i1 false)
  %arr.idx2E461 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %1, i64 0, i32 1, i64 2
  %50 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx2E461 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %50, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.196" to i8*), i64 32, i1 false)
  %arr.idx3E462 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %1, i64 0, i32 1, i64 3
  %51 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx3E462 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %51, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.197" to i8*), i64 32, i1 false)
  %arr.idx4E463 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %1, i64 0, i32 1, i64 4
  %52 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx4E463 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %52, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.198" to i8*), i64 32, i1 false)
  %arr.idx5E464 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %1, i64 0, i32 1, i64 5
  %53 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx5E464 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %53, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.199" to i8*), i64 32, i1 false)
  %arr.idx6E465 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %1, i64 0, i32 1, i64 6
  %54 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx6E465 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %54, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.200" to i8*), i64 32, i1 false)
  %arr.idx7E466 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %1, i64 0, i32 1, i64 7
  %55 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx7E466 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %55, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.201" to i8*), i64 32, i1 false)
  %arr.idx8E467 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %1, i64 0, i32 1, i64 8
  %56 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx8E467 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %56, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.202" to i8*), i64 32, i1 false)
  %arr.idx9E468 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %1, i64 0, i32 1, i64 9
  %57 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx9E468 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %57, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.203" to i8*), i64 32, i1 false)
  %58 = load i1, i1* getelementptr inbounds (%"record._ZN7default11std$FS$core6OptionIN_ZN11std$FS$core6OptionIT6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEEEE", %"record._ZN7default11std$FS$core6OptionIN_ZN11std$FS$core6OptionIT6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEEEE"* @_ZN7default22var_1698522025530_3296E, i64 0, i32 0), align 8
  %ifvalue472.sroa.4.0.ifvalue472.0..sroa_cast = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %ifvalue472.sroa.4 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %ifvalue472.sroa.4.0.ifvalue472.0..sroa_cast, i8 0, i64 120, i1 false)
  br i1 %58, label %if.else475, label %if.then476

if.else123:                                       ; preds = %landingPad
  call void @CJ_MCC_EndCatch()
  invoke void @CJ_MCC_ThrowException(i8 addrspace(1)* %29)
          to label %invoke.continue163 unwind label %finally.rethrow

invoke.continue163:                               ; preds = %if.else123
  unreachable

finally.rethrow:                                  ; preds = %if.else123
  %59 = landingpad token
          catch i8* null
  %60 = call i8* @CJ_MCC_GetExceptionWrapper()
  %61 = call i8 addrspace(1)* @CJ_MCC_BeginCatch(i8* %60)
  %enum.val184.sroa.246.0..sroa_cast48 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %enum.val184.sroa.246 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %enum.val184.sroa.246.0..sroa_cast48, i8 0, i64 120, i1 false)
  %.0..sroa_cast51 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %25 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %.0..sroa_cast51, i8 0, i64 120, i1 false)
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* noundef nonnull align 8 dereferenceable(120) %enum.val184.sroa.246.0..sroa_cast48, i8* noundef nonnull align 8 dereferenceable(120) %.0..sroa_cast51, i64 120, i1 false)
  %62 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %2, i64 0, i32 0, i32 1
  store i64 10, i64* %62, align 8
  %63 = bitcast { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %2 to i8**
  store i8* bitcast (%KlassInfo.0* @"_ZN11std$FS$core6StringE.arrayKlass" to i8*), i8** %63, align 8
  %64 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %2, i64 0, i32 1
  %65 = bitcast [10 x %"record._ZN11std$FS$core6StringE"]* %64 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %65, i8 0, i64 320, i1 false)
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %65, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.184" to i8*), i64 32, i1 false)
  %arr.idx1E271 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %2, i64 0, i32 1, i64 1
  %66 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx1E271 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %66, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.185" to i8*), i64 32, i1 false)
  %arr.idx2E272 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %2, i64 0, i32 1, i64 2
  %67 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx2E272 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %67, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.186" to i8*), i64 32, i1 false)
  %arr.idx3E273 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %2, i64 0, i32 1, i64 3
  %68 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx3E273 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %68, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.187" to i8*), i64 32, i1 false)
  %arr.idx4E274 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %2, i64 0, i32 1, i64 4
  %69 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx4E274 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %69, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.188" to i8*), i64 32, i1 false)
  %arr.idx5E275 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %2, i64 0, i32 1, i64 5
  %70 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx5E275 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %70, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.189" to i8*), i64 32, i1 false)
  %arr.idx6E276 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %2, i64 0, i32 1, i64 6
  %71 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx6E276 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %71, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.190" to i8*), i64 32, i1 false)
  %arr.idx7E277 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %2, i64 0, i32 1, i64 7
  %72 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx7E277 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %72, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.191" to i8*), i64 32, i1 false)
  %arr.idx8E278 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %2, i64 0, i32 1, i64 8
  %73 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx8E278 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %73, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.192" to i8*), i64 32, i1 false)
  %arr.idx9E279 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %2, i64 0, i32 1, i64 9
  %74 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx9E279 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %74, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.193" to i8*), i64 32, i1 false)
  %75 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %3, i64 0, i32 0, i32 1
  store i64 10, i64* %75, align 8
  %76 = bitcast { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %3 to i8**
  store i8* bitcast (%KlassInfo.0* @"_ZN11std$FS$core6StringE.arrayKlass" to i8*), i8** %76, align 8
  %77 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %3, i64 0, i32 1
  %78 = bitcast [10 x %"record._ZN11std$FS$core6StringE"]* %77 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %78, i8 0, i64 320, i1 false)
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %78, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.194" to i8*), i64 32, i1 false)
  %arr.idx1E294 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %3, i64 0, i32 1, i64 1
  %79 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx1E294 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %79, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.195" to i8*), i64 32, i1 false)
  %arr.idx2E295 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %3, i64 0, i32 1, i64 2
  %80 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx2E295 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %80, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.196" to i8*), i64 32, i1 false)
  %arr.idx3E296 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %3, i64 0, i32 1, i64 3
  %81 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx3E296 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %81, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.197" to i8*), i64 32, i1 false)
  %arr.idx4E297 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %3, i64 0, i32 1, i64 4
  %82 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx4E297 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %82, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.198" to i8*), i64 32, i1 false)
  %arr.idx5E298 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %3, i64 0, i32 1, i64 5
  %83 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx5E298 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %83, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.199" to i8*), i64 32, i1 false)
  %arr.idx6E299 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %3, i64 0, i32 1, i64 6
  %84 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx6E299 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %84, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.200" to i8*), i64 32, i1 false)
  %arr.idx7E300 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %3, i64 0, i32 1, i64 7
  %85 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx7E300 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %85, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.201" to i8*), i64 32, i1 false)
  %arr.idx8E301 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %3, i64 0, i32 1, i64 8
  %86 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx8E301 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %86, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.202" to i8*), i64 32, i1 false)
  %arr.idx9E302 = getelementptr inbounds { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }, { %ArrayBase, [10 x %"record._ZN11std$FS$core6StringE"] }* %3, i64 0, i32 1, i64 9
  %87 = bitcast %"record._ZN11std$FS$core6StringE"* %arr.idx9E302 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %87, i8* align 16 bitcast ({ { i8 addrspace(1)*, i64, i64 }, i64 }* @"$const_cjstring.203" to i8*), i64 32, i1 false)
  %88 = load i1, i1* getelementptr inbounds (%"record._ZN7default11std$FS$core6OptionIN_ZN11std$FS$core6OptionIT6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEEEE", %"record._ZN7default11std$FS$core6OptionIN_ZN11std$FS$core6OptionIT6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEEEE"* @_ZN7default22var_1698522025530_3296E, i64 0, i32 0), align 8
  %ifvalue305.sroa.4.0.ifvalue305.0..sroa_cast = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %ifvalue305.sroa.4 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %ifvalue305.sroa.4.0.ifvalue305.0..sroa_cast, i8 0, i64 120, i1 false)
  br i1 %88, label %if.else308, label %if.then309

if.then309:                                       ; preds = %finally.rethrow
  %x310.sroa.4.0.x310.0..sroa_cast175 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %x310.sroa.4 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %x310.sroa.4.0.x310.0..sroa_cast175, i8 0, i64 120, i1 false)
  br label %if.end307

if.else308:                                       ; preds = %finally.rethrow
  %89 = load i1, i1* @_ZN7default21var_1698522025530_552E, align 8
  %ifvalue312.sroa.5.0.ifvalue312.0..sroa_cast190 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %ifvalue312.sroa.5 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %ifvalue312.sroa.5.0.ifvalue312.0..sroa_cast190, i8 0, i64 120, i1 false)
  br i1 %89, label %if.then321, label %if.else320

if.then321:                                       ; preds = %if.else308
  %90 = load i1, i1* @_ZN7default22var_1698522025530_1884E, align 8
  %ifvalue322.sroa.5.0.ifvalue322.0..sroa_cast182 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %ifvalue322.sroa.5 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %ifvalue322.sroa.5.0.ifvalue322.0..sroa_cast182, i8 0, i64 120, i1 false)
  %enum.val326.sroa.3140.enum.val327.sroa.4 = select i1 %90, %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %enum.val326.sroa.3140, %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %enum.val327.sroa.4
  %. = select i1 %90, %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %19, %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %16
  %enum.val327.sroa.4.0..sroa_cast181 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %enum.val326.sroa.3140.enum.val327.sroa.4 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %enum.val327.sroa.4.0..sroa_cast181, i8 0, i64 120, i1 false)
  %.0..sroa_cast186 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %. to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %.0..sroa_cast186, i8 0, i64 120, i1 false)
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* noundef nonnull align 8 dereferenceable(120) %enum.val327.sroa.4.0..sroa_cast181, i8* noundef nonnull align 8 dereferenceable(120) %.0..sroa_cast186, i64 120, i1 false)
  br label %if.end319

if.else320:                                       ; preds = %if.else308
  %enum.val328.sroa.4.0..sroa_cast189 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %enum.val328.sroa.4 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %enum.val328.sroa.4.0..sroa_cast189, i8 0, i64 120, i1 false)
  %.0..sroa_cast194 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %13 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %.0..sroa_cast194, i8 0, i64 120, i1 false)
  br label %if.end319

if.end319:                                        ; preds = %if.else320, %if.then321
  %.0..sroa_cast194.sink661 = phi i8* [ %.0..sroa_cast194, %if.else320 ], [ %.0..sroa_cast186, %if.then321 ]
  %enum.val328.sroa.4.0..sroa_cast189.sink = phi i8* [ %enum.val328.sroa.4.0..sroa_cast189, %if.else320 ], [ %ifvalue322.sroa.5.0.ifvalue322.0..sroa_cast182, %if.then321 ]
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* noundef nonnull align 8 dereferenceable(120) %enum.val328.sroa.4.0..sroa_cast189.sink, i8* noundef nonnull align 8 dereferenceable(120) %.0..sroa_cast194.sink661, i64 120, i1 false)
  br label %if.end307

if.end307:                                        ; preds = %if.end319, %if.then309
  %.0..sroa_cast194.sink.sink = phi i8* [ %.0..sroa_cast194.sink661, %if.end319 ], [ bitcast (%"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* getelementptr inbounds (%"record._ZN7default11std$FS$core6OptionIN_ZN11std$FS$core6OptionIT6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEEEE", %"record._ZN7default11std$FS$core6OptionIN_ZN11std$FS$core6OptionIT6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEEEE"* @_ZN7default22var_1698522025530_3296E, i64 0, i32 1, i32 1) to i8*), %if.then309 ]
  %ifvalue312.sroa.5.0.ifvalue312.0..sroa_cast190.sink662 = phi i8* [ %ifvalue312.sroa.5.0.ifvalue312.0..sroa_cast190, %if.end319 ], [ %x310.sroa.4.0.x310.0..sroa_cast175, %if.then309 ]
  %ifvalue312.sroa.5.0.ifvalue312.0..sroa_cast190.sink = phi i8* [ %ifvalue312.sroa.5.0.ifvalue312.0..sroa_cast190, %if.end319 ], [ bitcast (%"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* getelementptr inbounds (%"record._ZN7default11std$FS$core6OptionIN_ZN11std$FS$core6OptionIT6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEEEE", %"record._ZN7default11std$FS$core6OptionIN_ZN11std$FS$core6OptionIT6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEEEE"* @_ZN7default22var_1698522025530_3296E, i64 0, i32 1, i32 1) to i8*), %if.then309 ]
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* noundef nonnull align 8 dereferenceable(120) %ifvalue312.sroa.5.0.ifvalue312.0..sroa_cast190.sink662, i8* noundef nonnull align 8 dereferenceable(120) %.0..sroa_cast194.sink.sink, i64 120, i1 false)
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* noundef nonnull align 8 dereferenceable(120) %ifvalue305.sroa.4.0.ifvalue305.0..sroa_cast, i8* noundef nonnull align 8 dereferenceable(120) %ifvalue312.sroa.5.0.ifvalue312.0..sroa_cast190.sink, i64 120, i1 false)
  call void @CJ_MCC_EndCatch()
  call void @CJ_MCC_ThrowException(i8 addrspace(1)* %61)
  unreachable

if.then476:                                       ; preds = %if.end129
  %x477.sroa.4.0.x477.0..sroa_cast346 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %x477.sroa.4 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %x477.sroa.4.0.x477.0..sroa_cast346, i8 0, i64 120, i1 false)
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* noundef nonnull align 8 dereferenceable(120) %x477.sroa.4.0.x477.0..sroa_cast346, i8* noundef nonnull align 8 dereferenceable(120) bitcast (%"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* getelementptr inbounds (%"record._ZN7default11std$FS$core6OptionIN_ZN11std$FS$core6OptionIT6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEEEE", %"record._ZN7default11std$FS$core6OptionIN_ZN11std$FS$core6OptionIT6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEEEE"* @_ZN7default22var_1698522025530_3296E, i64 0, i32 1, i32 1) to i8*), i64 120, i1 false)
  br label %if.end474

if.else475:                                       ; preds = %if.end129
  %91 = load i1, i1* @_ZN7default21var_1698522025530_552E, align 8
  %ifvalue479.sroa.5.0.ifvalue479.0..sroa_cast361 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %ifvalue479.sroa.5 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %ifvalue479.sroa.5.0.ifvalue479.0..sroa_cast361, i8 0, i64 120, i1 false)
  br i1 %91, label %if.then488, label %if.else487

if.then488:                                       ; preds = %if.else475
  %92 = load i1, i1* @_ZN7default22var_1698522025530_1884E, align 8
  %ifvalue489.sroa.5.0.ifvalue489.0..sroa_cast353 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %ifvalue489.sroa.5 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %ifvalue489.sroa.5.0.ifvalue489.0..sroa_cast353, i8 0, i64 120, i1 false)
  br i1 %92, label %if.then492, label %if.else491

if.then492:                                       ; preds = %if.then488
  %enum.val493.sroa.3311.0..sroa_cast = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %enum.val493.sroa.3311 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %enum.val493.sroa.3311.0..sroa_cast, i8 0, i64 120, i1 false)
  %.0..sroa_cast349 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %9 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %.0..sroa_cast349, i8 0, i64 120, i1 false)
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* noundef nonnull align 8 dereferenceable(120) %enum.val493.sroa.3311.0..sroa_cast, i8* noundef nonnull align 8 dereferenceable(120) %.0..sroa_cast349, i64 120, i1 false)
  br label %if.end490

if.else491:                                       ; preds = %if.then488
  %enum.val494.sroa.4.0..sroa_cast352 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %enum.val494.sroa.4 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %enum.val494.sroa.4.0..sroa_cast352, i8 0, i64 120, i1 false)
  %.0..sroa_cast357 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %7 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %.0..sroa_cast357, i8 0, i64 120, i1 false)
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* noundef nonnull align 8 dereferenceable(120) %enum.val494.sroa.4.0..sroa_cast352, i8* noundef nonnull align 8 dereferenceable(120) %.0..sroa_cast357, i64 120, i1 false)
  br label %if.end490

if.end490:                                        ; preds = %if.else491, %if.then492
  %.0..sroa_cast357.sink = phi i8* [ %.0..sroa_cast357, %if.else491 ], [ %.0..sroa_cast349, %if.then492 ]
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* noundef nonnull align 8 dereferenceable(120) %ifvalue489.sroa.5.0.ifvalue489.0..sroa_cast353, i8* noundef nonnull align 8 dereferenceable(120) %.0..sroa_cast357.sink, i64 120, i1 false)
  br label %if.end486

if.else487:                                       ; preds = %if.else475
  %enum.val495.sroa.4.0..sroa_cast360 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %enum.val495.sroa.4 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %enum.val495.sroa.4.0..sroa_cast360, i8 0, i64 120, i1 false)
  %.0..sroa_cast365 = bitcast %"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* %5 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %.0..sroa_cast365, i8 0, i64 120, i1 false)
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* noundef nonnull align 8 dereferenceable(120) %enum.val495.sroa.4.0..sroa_cast360, i8* noundef nonnull align 8 dereferenceable(120) %.0..sroa_cast365, i64 120, i1 false)
  br label %if.end486

if.end486:                                        ; preds = %if.else487, %if.end490
  %.0..sroa_cast365.sink = phi i8* [ %.0..sroa_cast365, %if.else487 ], [ %.0..sroa_cast357.sink, %if.end490 ]
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* noundef nonnull align 8 dereferenceable(120) %ifvalue479.sroa.5.0.ifvalue479.0..sroa_cast361, i8* noundef nonnull align 8 dereferenceable(120) %.0..sroa_cast365.sink, i64 120, i1 false)
  br label %if.end474

if.end474:                                        ; preds = %if.end486, %if.then476
  %ifvalue479.sroa.5.0.ifvalue479.0..sroa_cast361.sink = phi i8* [ %ifvalue479.sroa.5.0.ifvalue479.0..sroa_cast361, %if.end486 ], [ bitcast (%"T6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEE"* getelementptr inbounds (%"record._ZN7default11std$FS$core6OptionIN_ZN11std$FS$core6OptionIT6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEEEE", %"record._ZN7default11std$FS$core6OptionIN_ZN11std$FS$core6OptionIT6_R_ZN11std$FS$core6StringEltR_ZN11std$FS$core5ArrayImER_ZN11std$FS$core5RangeIhER_ZN11std$FS$core5ArrayImEEEE"* @_ZN7default22var_1698522025530_3296E, i64 0, i32 1, i32 1) to i8*), %if.then476 ]
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* noundef nonnull align 8 dereferenceable(120) %ifvalue472.sroa.4.0.ifvalue472.0..sroa_cast, i8* noundef nonnull align 8 dereferenceable(120) %ifvalue479.sroa.5.0.ifvalue479.0..sroa_cast361.sink, i64 120, i1 false)
  %93 = bitcast %"record._ZN7default11std$FS$core5RangeItE"* %0 to i8*
  %tryvalue.sroa.0.0..sroa_idx = getelementptr inbounds %"record._ZN7default11std$FS$core5RangeItE", %"record._ZN7default11std$FS$core5RangeItE"* %0, i64 0, i32 0
  store i16 117, i16* %tryvalue.sroa.0.0..sroa_idx, align 8
  %tryvalue.sroa.2.0..sroa_idx615 = getelementptr inbounds %"record._ZN7default11std$FS$core5RangeItE", %"record._ZN7default11std$FS$core5RangeItE"* %0, i64 0, i32 1
  store i16 11, i16* %tryvalue.sroa.2.0..sroa_idx615, align 2
  %tryvalue.sroa.3.0..sroa_idx = getelementptr inbounds i8, i8* %93, i64 4
  %tryvalue.sroa.3.0..sroa_cast = bitcast i8* %tryvalue.sroa.3.0..sroa_idx to i32*
  store i32 0, i32* %tryvalue.sroa.3.0..sroa_cast, align 4
  %tryvalue.sroa.4.0..sroa_idx616 = getelementptr inbounds %"record._ZN7default11std$FS$core5RangeItE", %"record._ZN7default11std$FS$core5RangeItE"* %0, i64 0, i32 2
  store i64 -21, i64* %tryvalue.sroa.4.0..sroa_idx616, align 8
  %tryvalue.sroa.5.0..sroa_idx617 = getelementptr inbounds %"record._ZN7default11std$FS$core5RangeItE", %"record._ZN7default11std$FS$core5RangeItE"* %0, i64 0, i32 3
  store i1 true, i1* %tryvalue.sroa.5.0..sroa_idx617, align 8
  %tryvalue.sroa.6.0..sroa_idx618 = getelementptr inbounds %"record._ZN7default11std$FS$core5RangeItE", %"record._ZN7default11std$FS$core5RangeItE"* %0, i64 0, i32 4
  store i1 true, i1* %tryvalue.sroa.6.0..sroa_idx618, align 1
  %tryvalue.sroa.7.0..sroa_idx619 = getelementptr inbounds %"record._ZN7default11std$FS$core5RangeItE", %"record._ZN7default11std$FS$core5RangeItE"* %0, i64 0, i32 5
  store i1 false, i1* %tryvalue.sroa.7.0..sroa_idx619, align 2
  %tryvalue.sroa.8.0..sroa_raw_idx = getelementptr inbounds i8, i8* %93, i64 19
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* noundef nonnull align 1 dereferenceable(5) %tryvalue.sroa.8.0..sroa_raw_idx, i8* noundef nonnull align 1 dereferenceable(5) %tryvalue.sroa.8657, i64 5, i1 false)
  ret void
}

attributes #0 = { "cj-runtime" "gc-leaf-function" }
