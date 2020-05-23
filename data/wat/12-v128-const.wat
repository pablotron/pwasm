;;
;; 12-v128-const.wat: Module containing one function to test v128.const.
;;
(module
  (func $i8x16_add (param $a i32)
                   (result i32)
    ;; extract the third lane
    (i8x16.extract_lane_u 3
      ;; add two i8x16s
      (i8x16.add
        ;; replace third lane
        (i8x16.replace_lane 3
          (v128.const i32x4 0x03020100 0x07060504 0x0b0a0908 0x0f0e0d0c)
          (local.get $a))
        (v128.const i32x4 0x03020100 0x07060504 0x0b0a0908 0x0f0e0d0c)))
  )

  (export "i8x16_add" (func $i8x16_add))
)
