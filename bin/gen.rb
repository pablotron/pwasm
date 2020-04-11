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
          const: to_const(row[:name]),
          flags: flags.ids,
          mask: flags.mask,
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
