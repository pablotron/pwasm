(module
  ;;
  ;; Calculate the length of the hypotenuse of a right triangle from the
  ;; lengths of the adjacent and opposite sides of the triangle.
  ;;
  ;; The lengths of the adjacent and opposite sides and the result are
  ;; 32-bit, single-precision floating point values.
  ;;
  (func $f32 (param $a f32) (param $b f32) (result f32)
    (f32.sqrt (f32.add
      (f32.mul (local.get $a) (local.get $a))
      (f32.mul (local.get $b) (local.get $b))
    ))
  )

  (export "f32" (func $f32))

  ;;
  ;; Calculate the length of the hypotenuse of a right triangle from the
  ;; lengths of the adjacent and opposite sides of the triangle.
  ;;
  ;; The lengths of the adjacent and opposite sides and the result are
  ;; 64-bit, double-precision floating point values.
  ;;
  (func $f64 (param $a f64) (param $b f64) (result f64)
    (f64.sqrt (f64.add
      (f64.mul (local.get $a) (local.get $a))
      (f64.mul (local.get $b) (local.get $b))
    ))
  )

  (export "f64" (func $f64))
)
