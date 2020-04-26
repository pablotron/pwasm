;;
;; 02-vec.wat: Vector functions.
;;
;; 2D Vector Functions:
;; * v2.mag: Calculate the magnitude of a 2D vector.
;; * v2.mul: Multiply a 2D vector by a scalar.
;; * v2.norm: Normalize a 2D vector.
;; * v2.dot: Calculate the dot product of two 2D vectors.
;; * v2.proj: Project a 2D vector onto another 2D vector.
;;
;; 3D Vector Functions:
;; * v3.mag: Calculate the magnitude of a 3D vector.
;; * v3.mul: Multiply a 3D vector by a scalar.
;; * v3.norm: Normalize a 3D vector.
;; * v3.dot: Calculate the dot product of two 3D vectors.
;; * v3.proj: Project a 3D vector onto another 2D vector.
;; * v3.cross: Calculate the cross product of two 3D vectors.
;;
;; Utility Functions:
;; * m2.det: Calculate the determinant of a 2x2 matrix.
;;
(module
  ;;
  ;; export 1 page of memory, which is used to write the results of
  ;; operations that return vectors.
  ;;
  (memory $mem (export "mem") 1 1)

  ;;
  ;; v2.store: Store a 2D vector at a memory address and return the
  ;; memory address.
  ;;
  (func $v2_store (param $dst i32) ;; destination address
                  (param $x f32) (param $y f32) ;; 2D vector
                  (result i32)
    (f32.store (local.get $dst) (local.get $x))
    (f32.store (i32.add (local.get $dst) (i32.const 4)) (local.get $y))
    (local.get $dst)
  )

  (export "v2.store" (func $v2_store))

  ;;
  ;; v2.mag: Calculate the magnitude of a 2D vector.
  ;;
  (func $v2_mag (param $src i32)
                (result f32)
    (local $x f32)
    (local $y f32)

    (local.set $x (f32.load (local.get $src)))
    (local.set $y (f32.load (i32.add (local.get $src) (i32.const 4))))

    (f32.sqrt
      (f32.add
        (f32.mul (local.get $x) (local.get $x))
        (f32.mul (local.get $y) (local.get $y))
      )
    )
  )

  (export "v2.mag" (func $v2_mag))

  ;;
  ;; v2.mul: Multiply a 2D vector by a scalar and return the memory
  ;; address of the vector.
  ;;
  (func $v2_mul (param $dst i32) ;; vector address
                (param $s f32) ;; scalar
                (result i32)
    (local $x f32)
    (local $y f32)

    ;; build result
    (local.set $x (f32.mul (local.get $s) (f32.load (local.get $dst))))
    (local.set $y (f32.mul (local.get $s) (f32.load (i32.add (local.get $dst) (i32.const 4)))))

    ;; write result to memory
    (call $v2_store (local.get $dst) (local.get $x) (local.get $y))
  )

  (export "v2.mul" (func $v2_mul))

  ;;
  ;; v2.norm: Normalize a 2D vector and return the memory address of the
  ;; vector.
  ;;
  (func $v2_norm (param $dst i32) ;; vector address
                 (result i32)
    (local $mag f32)
    (local $x f32)
    (local $y f32)

    (local.set $x (f32.load (local.get $dst)))
    (local.set $y (f32.load (i32.add (local.get $dst) (i32.const 4))))

    ;; get the magnitude of the vector
    (local.set $mag (call $v2_mag (local.get $dst)))

    ;; build result
    (local.set $x (f32.div (local.get $x) (local.get $mag)))
    (local.set $y (f32.div (local.get $y) (local.get $mag)))

    ;; write result to destination memory address, and then return
    ;; destination memory address
    (call $v2_store (local.get $dst) (local.get $x) (local.get $y))
  )

  (export "v2.norm" (func $v2_norm))

  ;;
  ;; v2.dot: Calculate dot product of two 2D vectors.
  ;;
  (func $v2_dot (param $a i32)
                (param $b i32)
                (result f32)
    (f32.add
      ;; a.x * b.x
      (f32.mul (f32.load (local.get $a)) (f32.load (local.get $b)))

      ;; a.y * b.y
      (f32.mul
        (f32.load (i32.add (local.get $a) (i32.const 4)))
        (f32.load (i32.add (local.get $b) (i32.const 4)))
      )
    )
  )

  (export "v2.dot" (func $v2_dot))

  ;;
  ;; v2.proj: Calculate the vector projection of A onto B, store the
  ;; result to the destination memory adddress, and then return the
  ;; destination memory address.
  ;;
  (func $v2_proj (param $dst i32) ;; destination address
                 (param $a i32) ;; Address of A
                 (param $b i32) ;; Address of B
                 (result i32)
    (local $b_mag f32)
    (local $s f32)
    (local $x f32)
    (local $y f32)

    ;; get magnitude of B
    (local.set $b_mag (call $v2_mag (local.get $b)))

    ;; calculate dot(a, b) / (|b|^2)
    (local.set $s (f32.div
      (call $v2_dot (local.get $a) (local.get $b))
      (f32.mul (local.get $b_mag) (local.get $b_mag))
    ))

    ;; build result
    (local.set $x (f32.mul (local.get $s) (f32.load (local.get $b))))
    (local.set $y (f32.mul (local.get $s) (f32.load (i32.add
      (local.get $b)
      (i32.const 4)
    ))))

    ;; write result to destination, then return destination
    (call $v2_store (local.get $dst) (local.get $x) (local.get $y))
  )

  (export "v2.proj" (func $v2_proj))

  ;;
  ;; v3.store: Store a 3D vector at the given memory address and
  ;; then return the memory address.
  ;;
  (func $v3_store (param $dst i32) ;; destination address
                  (param $x f32) (param $y f32) (param $z f32) ;; vector
                  (result i32)
    (f32.store (local.get $dst) (local.get $x))
    (f32.store (i32.add (local.get $dst) (i32.const 4)) (local.get $y))
    (f32.store (i32.add (local.get $dst) (i32.const 8)) (local.get $z))
    (local.get $dst)
  )

  (export "v3.store" (func $v3_store))

  ;;
  ;; v3.mag: Calculate the magnitude of a 3D vector.
  ;;
  (func $v3_mag (param $src i32)
                (result f32)
    (local $x f32)
    (local $y f32)
    (local $z f32)

    (local.set $x (f32.load (local.get $src)))
    (local.set $y (f32.load (i32.add (local.get $src) (i32.const 4))))
    (local.set $z (f32.load (i32.add (local.get $src) (i32.const 8))))

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

  (export "v3.mag" (func $v3_mag))

  ;;
  ;; v3.mul: Multiply a 3D vector by a scalar and return the memory
  ;; address.
  ;;
  (func $v3_mul (param $dst i32) ;; destination memory address
                (param $s f32) ;; scalar
                (result i32)
    (call $v3_store
      (local.get $dst)
      (f32.mul (local.get $s) (f32.load (local.get $dst)))
      (f32.mul (local.get $s) (f32.load (i32.add (local.get $dst) (i32.const 4))))
      (f32.mul (local.get $s) (f32.load (i32.add (local.get $dst) (i32.const 8))))
    )
  )

  (export "v3.mul" (func $v3_mul))

  ;;
  ;; v3.norm: Normalize 3D vector and then return the memory address.
  ;;
  (func $v3_norm (param $dst i32) ;; destination memory address
                 (result i32)
    ;; multiply vector by inverse magnitude, return destination
    (call $v3_mul
      ;; destination address
      (local.get $dst)

      ;; calculate inverse magnitude
      (f32.div (f32.const 1) (call $v3_mag (local.get $dst)))
    )
  )

  (export "v3.norm" (func $v3_norm))

  ;;
  ;; v3.dot: Calculate dot product of two 3D vectors.
  ;;
  (func $v3_dot (param $a i32)
                (param $b i32)
                (result f32)
    (f32.add
      (f32.add
        ;; a.x * b.x
        (f32.mul
          (f32.load (local.get $a))
          (f32.load (local.get $b))
        )

        ;; a.y * b.y
        (f32.mul
          (f32.load (i32.add (local.get $a) (i32.const 4)))
          (f32.load (i32.add (local.get $b) (i32.const 4)))
        )
      )

      ;; a.z * b.z
      (f32.mul
        (f32.load (i32.add (local.get $a) (i32.const 8)))
        (f32.load (i32.add (local.get $b) (i32.const 8)))
      )
    )
  )

  (export "v3.dot" (func $v3_dot))

  ;;
  ;; v3.proj: Calculate the vector projection of A onto B, write result
  ;; to the destination memory address, and return the destination
  ;; memory address.
  ;;
  (func $v3_proj (param $dst i32) ;; destination memory address
                 (param $a i32) ;; vector A
                 (param $b i32) ;; vector B
                 (result i32)
    (local $b_mag f32)
    (local $s f32)

    ;; get magnitude of B
    (local.set $b_mag (call $v3_mag (local.get $b)))

    ;; calculate scalar: dot(a, b) / (|b|^2)
    (local.set $s (f32.div
      (call $v3_dot (local.get $a) (local.get $b))
      (f32.mul (local.get $b_mag) (local.get $b_mag))
    ))

    ;; store result, return address
    (call $v3_store
      (local.get $dst)
      (f32.mul (local.get $s) (f32.load (local.get $b)))
      (f32.mul (local.get $s) (f32.load (i32.add (local.get $b) (i32.const 4))))
      (f32.mul (local.get $s) (f32.load (i32.add (local.get $b) (i32.const 8))))
    )
  )

  (export "v3.proj" (func $v3_proj))

  ;;
  ;; v3.cross: Calculate cross product of two 3D vectors, write result
  ;; to the destination memory address, then return the destination
  ;; memory address.
  ;;
  (func $v3_cross (param $dst i32) ;; destination memory address
                  (param $a i32) ;; address of vector A
                  (param $b i32) ;; address of vector B
                  (result i32)
    (local $ax f32)
    (local $ay f32)
    (local $az f32)
    (local $bx f32)
    (local $by f32)
    (local $bz f32)

    (local.set $ax (f32.load (local.get $a)))
    (local.set $ay (f32.load (i32.add (local.get $a) (i32.const 4))))
    (local.set $az (f32.load (i32.add (local.get $a) (i32.const 8))))
    (local.set $bx (f32.load (local.get $b)))
    (local.set $by (f32.load (i32.add (local.get $b) (i32.const 4))))
    (local.set $bz (f32.load (i32.add (local.get $b) (i32.const 8))))

    ;; write result to destination, then return destination
    (call $v3_store
      (local.get $dst)
      (call $m2_det (local.get $ay) (local.get $az)
                    (local.get $by) (local.get $bz))
      (call $m2_det (local.get $ax) (local.get $az)
                    (local.get $bx) (local.get $bz))
      (call $m2_det (local.get $ax) (local.get $ay)
                    (local.get $bx) (local.get $by))
    )
  )

  (export "v3.cross" (func $v3_cross))

  ;;
  ;; m2.det: Calculate the determinant of a 2x2 matrix.
  ;;
  (func $m2_det (param $v00 f32) (param $v01 f32)
                (param $v10 f32) (param $v11 f32)
                (result f32)
    ;;
    ;; given this 2x2 matrix:
    ;;   [ a b ]
    ;;   [ c d ]
    ;;
    ;; return:
    ;;   ad - bc
    ;;
    (f32.sub
      (f32.mul (local.get $v00) (local.get $v11))
      (f32.mul (local.get $v01) (local.get $v10))
    )
  )

  (export "m2.det" (func $m2_det))
)
