#!/usr/bin/env ruby

require 'csv'

#
# Generate 
#
module PWASM
  # path to ops.csv
  CSV_PATH = ENV.fetch(
    'PWASM_OPS_CSV_PATH',
    File.expand_path('../data/ops.csv', __dir__).freeze
  )

  #
  # CSV parser config.
  #
  CSV_CONFIG = {
    headers: true,
    header_converters: :symbol,
  }

  #
  # Minimal byte class.
  #
  class Byte
    attr :val, :str, :hex, :sym

    def initialize(val)
      @val = val
      @str = '%02X' % [@val]
      @hex = '0x%02X' % [@val]
      @sym = @str.intern
    end

    def to_s
      @str
    end
  end

  #
  # Build static array of all possible opcodes.
  #
  BYTES = (0..0xFF).map { |val|
    Byte.new(val)
  }.freeze

  #
  # Bitmask flag class.
  #
  class Flag
    attr :id, :shift, :mask

    def initialize(id, shift)
      @id = id
      @shift = shift
      @mask = (1 << shift)
    end
  end

  FLAG_IDS = %i{reserved const control mem global local}
  FLAGS = FLAG_IDS.each_with_index.map { |id, i|
    Flag.new(id, i)
  }.freeze

  #
  # Set of flags for an opcode.
  #
  class OpFlags
    attr :ids, :mask

    def initialize(s)
      @ids = (s || '').strip.split(/\s+/).map { |v| v.intern }
      @mask = FLAGS.reduce(0) do |r, flag|
        r |= @ids.include?(flag.id) ? flag.mask : 0
      end
    end
  end

  class OpsModel
    attr :rows, :bytes

    def initialize(path)
      @rows ||= CSV.new(File.open(path), **CSV_CONFIG).map do |row|
        row = row.to_h
        id = Byte.new(row[:id].to_i(16))
        flags = OpFlags.new(row[:flags])
        row.merge({
          id: id,
          const:  to_const(row[:name]),
          flags:  flags.ids,
          mask:   flags.mask,
          src:    to_src(row[:src]),
          dst:    to_src(row[:dst]),
        })
      end

      # build lut
      lut = @rows.each.with_object({}) do |row, r|
        r[row[:id].sym] = row
      end

      @bytes = BYTES.map do |id|
        lut[id.sym] || reserved(id)
      end
    end

    private

    R_FLAGS = OpFlags.new('reserved').freeze

    def reserved(id)
      { id: id, flags: R_FLAGS.ids, mask: R_FLAGS.mask }
    end

    def to_const(s)
      s.upcase.gsub('.', '_')
    end

    def to_src(s)
      (s || '').strip.split(/\s+/).map { |v| v.intern }
    end
  end

  #
  # Namespace for views (renderers).
  #
  module Views
    #
    # Base view class.
    #
    class View
      def self.run(model, args)
        new(model).run(args)
      end

      def initialize(model)
        @model = model
      end
    end

    #
    # Generate opcodes enum for header.
    #
    class OpsEnum < View
      DELIM = " \\\n"

      TEMPLATES = {
        wrap: '  /* 0x%<id>s */ %<text>s',
        default:  'PWASM_OP(%<const>s, "%<name>s", %<imm>s)',
        control:  'PWASM_OP_CONTROL(%<const>s, "%<name>s", %<imm>s)',
        const:    'PWASM_OP_CONST(%<const>s, "%<name>s", %<imm>s)',
        reserved: 'PWASM_OP_RESERVED(_%<id>s, "%<id>s")',
      }

      def run(args)
        @model.bytes.map { |row|
          TEMPLATES[:wrap] % row.merge({
            text: TEMPLATES[tmpl_key(row)] % row,
          })
        }.join(DELIM)
      end

      private

      #
      # Get template key for row.
      #
      def tmpl_key(row)
        TEMPLATES.keys.find { |k|
          row[:flags].include?(k)
        } || :default
      end
    end

    #
    # Generate flags array for opcodes.
    #
    class OpFlags < View
      # row delimiter
      DELIM = "\n"

      TEMPLATES = {
        layout: %[
          static const uint8_t
          PWASM_OP_FLAGS[] = {
            %<rows>s
          };
        ].gsub(/\n\s*/m, "\n").strip,

        wrap:     '  0x%<mask>02X, /* %<text>s */',
        default:  '%<name>s (%<flags>s)',
        reserved: '(reserved)',
      }

      def run(args)
        TEMPLATES[:layout] % {
          rows: @model.bytes.map { |row|
            key = row[:flags].include?(:reserved) ? :reserved : :default

            TEMPLATES[:wrap] % row.merge({
              text: TEMPLATES[key] % row.merge({
                flags: to_flags(row[:flags]),
              }),
            })
          }.join(DELIM)
        }
      end

      private

      #
      # Get template key for row
      #
      def tmpl_key(row)
        row[:flags].include?(:reserved) ? :reserved : :default
      end

      #
      # Build string representation of flags.
      #
      def to_flags(flags)
        (flags.size > 0) ? flags.join(', ') : 'none'
      end
    end

    #
    # Generate flags bitmask enum for header.
    #
    class FlagsEnum < View
      # row delimiter
      DELIM = "\n"

      TEMPLATES = {
        layout: %[
          typedef enum {
            %<rows>s
          } pwasm_op_flag_t;
        ].gsub(/\n\s*/m, "\n").strip,

        row: '  PWASM_OP_FLAG_%<const>s = (1 << %<shift>d),',
      }

      def run(args)
        TEMPLATES[:layout] % {
          rows: FLAGS.map { |row|
            TEMPLATES[:row] % {
              const: row.id.upcase,
              shift: row.shift,
            }
          }.join(DELIM)
        }
      end
    end

    #
    # Generate check function for instruction sequence.
    #
    class Check < View
      # row delimiter
      DELIM = "\n"

      TEMPLATES = {
        layout: %[
          typedef enum {
            PWASM_CHECK_TYPE_I32,
            PWASM_CHECK_TYPE_I64,
            PWASM_CHECK_TYPE_I64,
            PWASM_CHECK_TYPE_F32,
            PWASM_CHECK_TYPE_F64,
            PWASM_CHECK_TYPE_UNKNOWN,
            PWASM_CHECK_TYPE_LAST,
          } pwasm_check_type_t;

          static inline check_type_t
          pwasm_value_type_to_check_type(
            const pwasm_value_type_t type
          ) {
            switch (type) {



          static inline bool
          pwasm_check_insts(
            const pwasm_mod_t * const mod,
            const uint32_t func_id,
            const pwasm_slice_t insts,
            void (*on_error)(const char *, void *),
            void *cb_data
          ) {
            for (size_t i = 0; i < insts.len; i++) {
              const pwasm_inst_t in = insts\[i\];

              switch (in.op) {
              %<rows>s
              }
            }

            // return success
            return true;
          }
        ].gsub(/\n\s{10}/m, "\n").strip,

        row: %[
          case PWASM_OP_%<const>s:
            {
              %<code>s
            }
            break;
        ].gsub(/\n\s{6}/m, "\n").rstrip,
      }

      def run(args)
        TEMPLATES[:layout] % {
          rows: @model.rows.map { |row|
            TEMPLATES[:row] % row.merge({
              code: get_code(row),
            })
          }.join(DELIM)
        }
      end

      private

      OP_CHECKS = {
        flags: {
          # block, loop, if
          enter: %[
            // add label to control stack
            CTRL_PUSH(mod, in, OP_DEPTH());
          ].strip,

          # br, br_if
          br: %[
            // check label against control stack depth
            if (in.v_index.id >= CTRL_DEPTH()) {
              FAIL("branch target out of bounds");
            }
          ].strip,

          local: %[
            // get check type for local index, check for error
            pwasm_check_type_t type;
            if (!pwasm_local_get_check_type(mod, func_id, in.v_index.id, &type, on_error, cb_data)) {
              return 0;
            }
          ].strip,

          global: %[
            // get check type for global index, check for error
            pwasm_check_type_t type;
            if (!pwasm_global_get_check_type(mod, in.v_index.id, &type, on_error, cb_data)) {
              return 0;
            }
          ].strip,
        },

        ops: {
          'unreachable' => %[
            // flag top label as unreachable
            CTRL_PEEK(0).unreachable = true;
          ].strip,

          'else' => %[
            // get instruction and depth
            const pwasm_inst_t in = CTRL_PEEK(0).in;
            const size_t depth = CTRL_PEEK(0).depth;

            // check for "if" block
            if (in.op != PWASM_OP_IF) {
              FAIL("else without associated if");
            }

            // TODO: check results
            if (in.v_block.result_type != 0x40) {
              const pwasm_check_type_t check_type = pwasm_result_type_get_check_type(in.v_block.result_type);
              if (OP_PEEK(0) != check_type) {
                FAIL("result type mismatch before else");
              }
            }

            // fix op stack and depth
            CTRL_PEEK(0).in.op = PWASM_OP_ELSE;
            OP_SET_DEPTH(CTRL_PEEK(0).depth);
          ].strip,

          # TODO
          'end' => %[
            // TODO
          ].strip,

          # TODO
          'br' => %[
            // flag top label as unreachable
            CTRL_PEEK(0).unreachable = true;

            // TODO: pop operands
          ].strip,

          # TODO
          'br_table' => %[
            const uint32_t len = in->v_br_table.slice.len;
            const size_t max_depth = CTRL_DEPTH();

            for (size_t j = 0; j < len; j++) {
              const size_t label_ofs = in->v_br_table.slice.ofs + j;
              const uint32_t label = mod->u32s\[label_ofs\];

              // check label depth
              if (label >= max_depth) {
                FAIL("branch target out of bounds");
              }

              // TODO: check label type
            }
          ].strip,

          # TODO
          'return' => %[
            // TODO
          ].strip,

          # TODO
          'call' => %[
            // TODO
          ].strip,

          # TODO
          'call_indirect' => %[
            // TODO
          ].strip,

          'select' => %[
            // check stack depth
            if (OP_DEPTH() < 2) {
              FAIL("select operand missing");
            }

            // check operand types
            if (OP_PEEK(0) != OP_PEEK(1)) {
              FAIL("select operand type mismatch");
            }

            // pop operand from stack
            OP_POP_ANY();
          ].strip,

          'local.get' => %[
            // push type
            OP_PUSH(type);
          ].strip,

          'local.set' => %[
            // pop type
            OP_POP(type);
          ].strip,

          'local.tee' => %[
            // push/pop type
            OP_POP(type);
            OP_PUSH(type);
          ].strip,

          'global.get' => %[
            // push type
            OP_PUSH(type);
          ].strip,

          'global.set' => %[
            // pop type
            OP_POP(type);
          ].strip,
        },
      }

      def get_code(row)
        # pop operands
        (
          # pop args
          row[:src].reverse.map { |type| op_pop(type) } +

          # push results
          row[:dst].map { |type| op_push(type) } +

          # add custom flag logic
          row[:flags].map { |flag| OP_CHECKS[:flags][flag] || '' } +

          # add custom op logic
          [OP_CHECKS[:ops][row[:name]] || '']
        ).join("\n")
      end

      def op_pop(type)
        case type
        when :any
          'OP_POP_ANY();'
        else
          'OP_STACK_POP(%s);' % [type_name(type)]
        end
      end

      def op_push(type)
        'OP_PUSH(%s);' % [type_name(type)]
      end

      def type_name(s)
        'CHECK_TYPE_%s' % [s.upcase]
      end
    end
  end

  #
  # Module containing command-line interface.
  #
  module CLI
    #
    # Command-line actions.
    #
    # Each action in the command-line interface is represented as a
    # subclass of the +Action+ base class in this namespace.
    #
    module Actions
      ACTIONS = []

      #
      # Base command-line action class.
      #
      class Action
        def self.run(app, args)
          new(app).run(args)
        end

        def initialize(app)
          @app = app
        end

        def run(args)
          fail "not implemented"
        end

        protected

        def self.inherited(sub)
          # append subclasses to list of available actions
          ACTIONS << sub
        end

        #
        # Lazy-load ops model instance.
        #
        def ops_model
          @ops_model ||= OpsModel.new(CSV_PATH)
        end
      end

      #
      # Generate opcodes enum for header.
      #
      class OpsEnum < Action
        NAME = 'ops-enum'

        def run(args)
          puts Views::OpsEnum.run(ops_model, args)
        end
      end

      #
      # Generate flags array for opcodes.
      #
      class OpFlags < Action
        NAME = 'op-flags'

        def run(args)
          puts Views::OpFlags.run(ops_model, args)
        end
      end

      #
      # Generate flags bitmask enum for header.
      #
      class FlagsEnum < Action
        NAME = 'flags-enum'

        def run(args)
          puts Views::FlagsEnum.run(ops_model, args)
        end
      end

      #
      # Generate validation function for instruction sequence.
      #
      class Check < Action
        NAME = 'check'

        def run(args)
          puts Views::Check.run(ops_model, args)
        end
      end

      #
      # Default action for unknown commands.
      #
      class Unknown < Action
        NAME = 'unknown'

        #
        # Raise an "unknown mode" exception if an argument was provided,
        # or print brief usage instructions there were no arguments
        # provided.
        #
        def run(args)
          if args.size > 0
            fail "unknown mode: #{args.first}"
          else
            warn "Usage: #@app [mode]"
            exit -1
          end
        end
      end

      #
      # Find action matching given arguments.
      #
      # Returns Unknown action if no matching action exists.
      #
      def self.find(args)
        # find matching action, or default to Unknown
        Actions::ACTIONS.find { |row|
          row.const_get(:NAME) == args.first
        } || Unknown
      end
    end

    #
    # Command-line entry point.
    #
    def self.run(app, args)
      Actions.find(args).run(app, args)
    end
  end
end

# allow command-line invocation
PWASM::CLI.run($0, ARGV) if __FILE__ == $0
