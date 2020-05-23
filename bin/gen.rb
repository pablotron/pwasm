#!/usr/bin/env ruby

#
# gen.rb: Generate code from data in ops.yaml.
#
# Use "gen.rb help" for usage.
#

require 'yaml'
require 'pp'

module PWASM
  # path to ops.yaml
  CONFIG_PATH = ENV.fetch(
    'PWASM_CONFIG_PATH',
    File.expand_path('../data/ops.yaml', __dir__).freeze
  ).freeze

  #
  # Miscellaneous utility methods.
  #
  module Util
    #
    # Build constant name
    #
    def self.const(s)
      s.upcase.gsub('.', '_')
    end

    #
    # Convert integer to LEB128.
    #
    def self.leb128(v)
      r = []
      loop do
        more = (v > 0x7F)
        r << ((more ? (1 << 7) : 0) | (v & 0x7F))
        break unless more
        v = (v >> 7)
      end
      r
    end
  end

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
  # Opcode.
  #
  class Op
    attr :set
    attr :code
    attr :bytes
    attr :num_lanes
    attr :mem_size
    attr :val
    attr :const

    #
    # Create opcode instance from parent set and row from ops.yaml.
    #
    def initialize(set, row)
      # cache set and row
      @set = set
      @row = row

      # get opcode, name, immediate, memory size, and num_lanes
      @code = get_code(@row['code'])
      @name = row['name']
      @imm = @row.fetch('imm', 'NONE')
      @mem_size = @row.fetch('mem_size', 0)
      @num_lanes = @row.fetch('num_lanes', 0)

      # get code value
      @val = @code.to_i(16)

      # build constant name
      @const = Util.const(@row['name'])
      @bytes = get_bytes(@set, @val)

      # build hash
      @hash = {
        code: @code,
        val: @val,
        name: @name,
        const: @const,
        imm: @imm,
        mem_size: @mem_size,
        num_lanes: @num_lanes,
        bytes: @bytes,
      }.freeze
    end

    #
    # Return opcode as hash.
    #
    def to_hash
      @hash
    end

    private

    #
    # Sanitize opcode text.
    #
    def get_code(s)
      s.gsub(/0x(..)/) { "0x#{$1.upcase}" }.freeze
    end

    #
    # Get opcode bytes.
    #
    def get_bytes(set, val)
      set.bytes + (case set.encoding
      when :byte
        [val]
      when :leb128
        Util.leb128(val)
      else
        raise "unknown encoding"
      end)
    end
  end

  #
  # Set of opcodes, including their prefix and encoding method.
  #
  class OpSet
    attr :name
    attr :encoding
    attr :ops
    attr :bytes
    attr :const
    attr :prefix

    #
    # Create new opcode set from row in ops.yaml.
    #
    def initialize(set)
      # cache row
      @set = set.freeze

      # get name, prefix, and encoding
      @name = set['name']
      @prefix = set.fetch('prefix', '0x00')
      @encoding = set['encoding'].intern

      @const = Util.const(@name)

      # build prefix bytes
      byte = @prefix.to_i(16)
      @bytes = (byte != 0) ? [byte] : []

      # parse ops
      @ops = set['ops'].map { |row| Op.new(self, row) }

      # calculate maximum value
      @max_val = @ops.reduce(0) { |r, op| (op.val > r) ? op.val : r }

      @hash = {
        name: @name,
        max_val: @max_val,
        const: @const,
        prefix: @prefix,
        encoding: @encoding,
      }.freeze
    end

    def to_hash
      @hash
    end
  end

  class Model
    attr :config
    attr :ops
    attr :masks
    attr :sets
    attr :byte_map

    def initialize(config)
      @config = config.freeze
      @sets = @config['sets'].map { |set|
        OpSet.new(set)
      }.freeze

      # build full list of ops
      @ops = @sets.each.with_object([]) do |set, r|
        r.concat(set.ops)
      end.freeze

      # build full list of mask values
      @masks = @sets.each.with_object([]) do |set, r|
        # build lut
        lut = set.ops.each.with_object({}) { |op, r| r[op.code] = true }

        r.concat(4.times.map do |i|
          {
            set_name: set.name,
            ofs: i,
            val: 64.times.reduce(0) do |r, j|
              byte = BYTES[64 * i + j].hex
              # pp({ byte: byte, val: lut[byte], set: set.name })
              r |= (lut[byte] ? (1 << j) : 0)
            end,
          }
        end)
      end

      @byte_map = @sets.each.with_object([]) do |set, r|
        r.concat(BYTES.map { |byte|
          { byte: byte, op: set.ops.find { |op| op.val == byte.val } }
        })
      end.freeze
    end
  end

  #
  # Namespace for views (renderers).
  #
  module Views
    #
    # Abstract base view class.
    #
    class View
      def self.run(model)
        new(model).run
      end

      def initialize(model)
        @model = model
      end

      #
      # Virtual method; sub-classes should override this method.
      #
      def run
        raise "not implemented"
      end
    end

    #
    # Generate op set enumeration.
    #
    class OpSetEnum < View
      DELIM = "\n"

      TEMPLATES = {
        wrap: %{typedef enum {\n%<rows>s\n} pwasm_ops_t;},
        row:  %{  PWASM_OPS_%<const>s, /**< %<name>s */},
      }.freeze

      def run
        TEMPLATES[:wrap] % {
          rows: (@model.sets.map { |set| set.to_hash } + [{
            name: 'sentinel',
            const: 'LAST', # sentinel
          }]).map { |row|
            TEMPLATES[:row] % row
          }.join(DELIM),
        }
      end
    end


    #
    # Generate enumeration of opcodes.
    #
    class OpEnum < View
      DELIM = "\n"

      TEMPLATES = {
        wrap: %{typedef enum {\n%<rows>s\n} pwasm_op_t;},
        row:  %{  PWASM_OP_%<const>s, /**< %<name>s */},
      }.freeze

      def run
        TEMPLATES[:wrap] % {
          rows: (@model.ops.map { |op| op.to_hash } + [{
            name: 'sentinel',
            const: 'LAST', # sentinel
          }]).map { |row|
            TEMPLATES[:row] % row
          }.join(DELIM),
        }
      end
    end


    #
    # Generate opcodes define for header.
    #
    # TODO: move away from this and generate bare enum instead.
    #
    class OpDefs < View
      DELIM = " \\\n"

      TEMPLATES = {
        defs: %{#define PWASM_OP_DEFS \\\n%<rows>s},
        row:  %{  PWASM_OP(%<const>s, "%<name>s", %<imm>s)},
      }.freeze

      def run
        TEMPLATES[:defs] % {
          rows: @model.ops.map { |op|
            TEMPLATES[:row] % op.to_hash
          }.join(DELIM),
        }
      end
    end

    #
    # Generate enumeration containing bitmask of valid opcodes bytes.
    #
    class ValidOpMask < View
      DELIM = "\n"

      TEMPLATES = {
        wrap: %[static const uint64_t %<name>s[] = {\n%<rows>s\n};],
        row: %{  0x%<val>016x, // %<set_name>s[%<ofs>d]},
      }.freeze

      # output name
      NAME = 'PWASM_VALID_OPS_MASK'

      def run
        TEMPLATES[:wrap] % {
          name: NAME,
          rows: @model.masks.map { |row|
            TEMPLATES[:row] % row
          }.join(DELIM),
        }
      end
    end

    #
    # Generate array mapping bytes to pwasm_op_ts.
    #
    class OpMap < View
      DELIM = "\n"

      TEMPLATES = {
        wrap: %[static const pwasm_op_t\n%<name>s[] = {\n%<rows>s\n};],
        row: %{  PWASM_OP_%<name>s, // %<val>s},
      }.freeze

      # output name
      NAME = 'PWASM_OPS_MAP'

      def run
        TEMPLATES[:wrap] % {
          name: NAME,
          rows: @model.byte_map.map { |row|
            TEMPLATES[:row] % {
              val: row[:byte].hex,
              name: row[:op] ? row[:op].const : 'LAST'
            }
          }.join(DELIM),
        }
      end
    end

    #
    # Generate opcode data array.
    #
    class OpData < View
      DELIM = ", "

      TEMPLATES = {
        wrap: %(
// generated by: bin/gen.rb op-data
static const pwasm_op_data_t
%<name>s[] = {%<rows>s};
        ).strip,
        row: %(
{
  .set        = PWASM_OPS_%<set>s,
  .name       = "%<name>s",
  .bytes      = { %<bytes>s },
  .num_bytes  = %<num_bytes>d,
  .imm        = PWASM_IMM_%<imm>s,
  .mem_size   = %<mem_size>d,
  .num_lanes  = %<num_lanes>d,
}
        ).strip,
      }.freeze

      # output name
      NAME = 'PWASM_OPS'

      def run
        TEMPLATES[:wrap] % {
          name: NAME,
          rows: @model.ops.map { |op|
            bytes = op.bytes.map { |b| '0x%02x' % [b] }
            TEMPLATES[:row] % op.to_hash.merge({
              set:        op.set.const,
              bytes:      bytes.join(', '),
              num_bytes:  bytes.size,
            })
          }.join(DELIM),
        }
      end
    end

    #
    # Generate op set data array.
    #
    class OpSetData < View
      DELIM = ", "

      TEMPLATES = {
        wrap: %(
// generated by: bin/gen.rb set-data
static const struct {
  const char * const name; /**< set name */
  const size_t max_val; /**< maximum opcode value */
  const bool prefix; /**< prefix byte, or 0x00 for no prefix */
  const bool is_byte; /**< 1: opcodes are bytes, 0: opcodes are leb128s */
} %<name>s[] = {%<rows>s};
        ).strip,
        row: %(
{
  .name     = "%<name>s",
  .max_val  = %<max_val>d,
  .prefix   = %<prefix>s,
  .is_byte  = %<is_byte>s,
}
        ).strip,
      }.freeze

      # output name
      NAME = 'PWASM_OP_SETS'

      def run
        TEMPLATES[:wrap] % {
          name: NAME,
          rows: @model.sets.map { |set|
            TEMPLATES[:row] % set.to_hash.merge({
              is_byte: (set.encoding == :byte) ? 'true' : 'false',
            })
          }.join(DELIM),
        }
      end
    end
  end

  #
  # Command-line interface.
  #
  class CLI
    TEMPLATES = {
      help: "Commands:\n%<rows>s",
      row:  '  %<name>s: %<text>s',
    }

    COMMANDS = {
      'set-enum' => {
        text: 'Print header pwasm_ops_t enumeration.',
        func: proc { |model| Views::OpSetEnum.run(model) },
      },

      'set-data' => {
        text: 'Print source PWASM_OPS_DATA for source.',
        func: proc { |model| Views::OpSetData.run(model) },
      },

      'op-enum' => {
        text: 'Print header pwasm_op_t enumeration.',
        func: proc { |model| Views::OpEnum.run(model) },
      },

      'op-defs' => {
        text: 'Print header pwasm_op_t #define.',
        func: proc { |model| Views::OpDefs.run(model) },
      },

      'op-mask' => {
        text: 'Print valid ops bitmask for source file.',
        func: proc { |model| Views::ValidOpMask.run(model) },
      },

      'op-map' => {
        text: 'Print byte to op map for source file.',
        func: proc { |model| Views::OpMap.run(model) },
      },

      'op-data' => {
        text: 'Print PWASM_OPS array for source file.',
        func: proc { |model| Views::OpData.run(model) },
      },

      'help' => {
        text: 'Print help.',
        func: proc { |model|
          TEMPLATES[:help] % {
            rows: COMMANDS.keys.sort.map { |name|
              TEMPLATES[:row] % {
                name: name,
                text: COMMANDS[name][:text],
              }
            }.join("\n")
          }
        },
      },
    }

    #
    # Entry point for command-line interface.
    #
    def self.run(app, args)
      new.run(app, args)
    end

    #
    # Run command.
    #
    def run(app, args)
      # check command-line arguments
      check_args(args)
      args = %w{help} unless args.size > 0

      # read config, load model
      config = ::YAML.load(File.read(CONFIG_PATH))
      model = Model.new(config)

      # run commands
      args.each do |arg|
        puts COMMANDS[arg][:func].call(model)
      end
    end

    private

    #
    # Check command-line arguments for validity.
    def check_args(args)
      # check for invalid commands
      bad = args.select { |arg| !COMMANDS.key?(arg) }
      if bad.size > 0
        warn "Invalid commands (see 'help'): #{bad}"
        exit -1
      end
    end
  end
end

PWASM::CLI.run($0, ARGV) if __FILE__ == $0
