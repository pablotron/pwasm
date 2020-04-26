;;
;; 02-vec.wat: Module containing two functions which calculate the Nth
;; value of the Fibonacci sequence:
;;
;; * fib_recurse(i32) -> i32: Calculate the Nth value of the Fibonacci
;;   sequence, recursively.
;;
;; * fib_iterate(i32) -> i32: Calculate the Nth value of the Fibonacci
;;   sequence, iteratively.
;;
(module
  ;; f32.v3_dot: calculate magnitude of a 3-element f32 vector.
  (func $f32_v3_mag (param $x f32) (param $y f32) (param $z f32)
                    (result f32)
    (f32.sqrt 
      (f32.add 
        (f32.add
          (f32.mul (local.get $x) (local.get $x))
          (f32.mul (local.get $y) (local.get $y))
        )
        (f32.mul (local.get $z) (local.get $z))
      )
    )
  )

  (export "f32.v3_mag" (func $f32_v3_mag))

  ;; f32.v3_dot: calculate dot product of two 3-element f32 vectors.
  (func $f32_v3_dot (param $ax f32) (param $ay f32) (param $az f32)
                    (param $bx f32) (param $by f32) (param $bz f32)
                    (result f32)
    (f32.add
      (f32.add
        ;; a.x * b.x
        (f32.mul (local.get $ax) (local.get $bx))

        ;; a.y * b.y
        (f32.mul (local.get $ay) (local.get $by))
      )

      ;; a.z * b.z
      (f32.mul (local.get $az) (local.get $bz))
    )
  )

  (export "f32.v3_dot" (func $f32_v3_dot))
)
