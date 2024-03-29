require 'dtrace/helper'

module DTrace
  class TestSingletonFunctionEntry < TestCase
    def test_entry
      probe = <<-eoprobe
ruby$target:::function-entry
/strstr(copyinstr(arg0), "Foo") != NULL/
{
  printf("%s %s %s %d\\n", copyinstr(arg0), copyinstr(arg1), copyinstr(arg2), arg3);
}
      eoprobe

      trap_probe(probe, ruby_program) { |d_file, rb_file, probes|
	foo_calls = probes.map { |line| line.split }.find_all { |row|
	  row.first == '#<Class:Foo>'  && row[1] == 'foo'
	}

	assert_equal 10, foo_calls.length
	line = '2'
	foo_calls.each { |f| assert_equal line, f[3] }
	foo_calls.each { |f| assert_equal rb_file, f[2] }
      }
    end

    def test_exit
      probe = <<-eoprobe
ruby$target:::function-return
{
  printf("%s %s %s %d\\n", copyinstr(arg0), copyinstr(arg1), copyinstr(arg2), arg3);
}
      eoprobe

      trap_probe(probe, ruby_program) { |d_file, rb_file, probes|
	foo_calls = probes.map { |line| line.split }.find_all { |row|
	  row.first == '#<Class:Foo>'  && row[1] == 'foo'
	}

	assert_equal 10, foo_calls.length
	line = '2'
	foo_calls.each { |f| assert_equal line, f[3] }
	foo_calls.each { |f| assert_equal rb_file, f[2] }
      }
    end

    def ruby_program
      <<-eoruby
      class Foo
	def self.foo; end
      end
      10.times { Foo.foo }
      eoruby
    end
  end
end
