; Complete fixture generated from fade_primitive_validation.hlsl with
; dxc 1.9.2602.24 (the version pinned by build_windows.ps1):
;   dxc -T ps_6_0 -E main -Od -Fo fixture.dxil fade_primitive_validation.hlsl
;   dxc -dumpbin -Fc fixture.ll fixture.dxil
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

%dx.types.Handle = type { i8* }
%dx.types.CBufRet.f32 = type { float, float, float, float }
%FadeConstants = type { float, float }

@thresholds = internal unnamed_addr constant [9 x float] [float 0.000000e+00, float 5.000000e-01, float 1.250000e-01, float 6.250000e-01, float 2.500000e-01, float 7.500000e-01, float 8.750000e-01, float 3.750000e-01, float 1.000000e+00], align 4
@dx.nothing.a = internal constant [1 x i32] zeroinitializer

define void @main() {
  %1 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 0, i32 0, i1 false)
  %2 = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 0, i32 undef)
  %3 = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 1, i32 undef)
  %4 = load i32, i32* getelementptr inbounds ([1 x i32], [1 x i32]* @dx.nothing.a, i32 0, i32 0)
  %5 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %1, i32 0)
  %6 = extractvalue %dx.types.CBufRet.f32 %5, 0
  %7 = fcmp fast ogt float %6, 0.000000e+00
  %8 = icmp ne i1 %7, false
  %9 = icmp ne i1 %8, false
  br i1 %9, label %10, label %30, !dx.controlflow.hints !18

; <label>:10
  %11 = fptosi float %2 to i32
  %12 = srem i32 %11, 3
  %13 = load i32, i32* getelementptr inbounds ([1 x i32], [1 x i32]* @dx.nothing.a, i32 0, i32 0)
  %14 = fptosi float %3 to i32
  %15 = srem i32 %14, 3
  %16 = load i32, i32* getelementptr inbounds ([1 x i32], [1 x i32]* @dx.nothing.a, i32 0, i32 0)
  %17 = mul nsw i32 %12, 3
  %18 = add nsw i32 %17, %15
  %19 = getelementptr inbounds [9 x float], [9 x float]* @thresholds, i32 0, i32 %18
  %20 = load float, float* %19, align 4
  %21 = load i32, i32* getelementptr inbounds ([1 x i32], [1 x i32]* @dx.nothing.a, i32 0, i32 0)
  %22 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %1, i32 0)
  %23 = extractvalue %dx.types.CBufRet.f32 %22, 1
  %24 = fmul fast float %23, 2.000000e+00
  %25 = fsub fast float %24, %20
  %26 = call float @dx.op.binary.f32(i32 35, float %25, float 0.000000e+00)  ; FMax(a,b)
  %27 = call float @dx.op.binary.f32(i32 36, float %26, float 1.000000e+00)  ; FMin(a,b)
  %28 = fadd fast float %27, 0x3FD54FDF40000000
  %29 = load i32, i32* getelementptr inbounds ([1 x i32], [1 x i32]* @dx.nothing.a, i32 0, i32 0)
  br label %30

; <label>:30
  %31 = phi float [ %28, %10 ], [ 1.000000e+00, %0 ]
  %32 = fsub fast float %31, 5.000000e-01
  %33 = fcmp fast olt float %32, 0.000000e+00
  call void @dx.op.discard(i32 82, i1 %33)
  %34 = load i32, i32* getelementptr inbounds ([1 x i32], [1 x i32]* @dx.nothing.a, i32 0, i32 0)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 0, float 1.000000e+00)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 1, float 1.000000e+00)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 2, float 1.000000e+00)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float %31)
  ret void
}

declare float @dx.op.loadInput.f32(i32, i32, i32, i8, i32) #0
declare void @dx.op.storeOutput.f32(i32, i32, i32, i8, float) #1
declare float @dx.op.binary.f32(i32, float, float) #0
declare void @dx.op.discard(i32, i1) #1
declare %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32, %dx.types.Handle, i32) #2
declare %dx.types.Handle @dx.op.createHandle(i32, i8, i32, i32, i1) #2

attributes #0 = { nounwind readnone }
attributes #1 = { nounwind }
attributes #2 = { nounwind readonly }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!2}
!dx.shaderModel = !{!3}
!dx.resources = !{!4}
!dx.viewIdState = !{!7}
!dx.entryPoints = !{!8}
!0 = !{!"dxcoob 1.9.2602.24 (d355aa836)"}
!1 = !{i32 1, i32 0}
!2 = !{i32 1, i32 9}
!3 = !{!"ps", i32 6, i32 0}
!4 = !{null, null, !5, null}
!5 = !{!6}
!6 = !{i32 0, %FadeConstants* undef, !"", i32 0, i32 0, i32 1, i32 8, null}
!7 = !{[6 x i32] [i32 4, i32 4, i32 8, i32 8, i32 0, i32 0]}
!8 = !{void ()* @main, !"main", !9, !4, !17}
!9 = !{!10, !14, null}
!10 = !{!11}
!11 = !{i32 0, !"SV_Position", i8 9, i8 3, !12, i8 4, i32 1, i8 4, i32 0, i8 0, !13}
!12 = !{i32 0}
!13 = !{i32 3, i32 3}
!14 = !{!15}
!15 = !{i32 0, !"SV_Target", i8 9, i8 16, !12, i8 0, i32 1, i8 4, i32 0, i8 0, !16}
!16 = !{i32 3, i32 15}
!17 = !{i32 0, i64 1}
!18 = distinct !{!18, !"dx.controlflow.hints", i32 1}
