describe 'database' do
  before(:each) { system("rm -f test.db") }
  after(:each)  { system("rm -f test.db") }

  def run_script(commands)
    raw_output = nil
    IO.popen("./db test.db", "r+") do |pipe|
      commands.each { |cmd| pipe.puts cmd }
      pipe.close_write
      raw_output = pipe.gets(nil)
    end
    raw_output.split("\n")
  end

  it 'inserts and retrieves a row' do
    result = run_script([
      "insert 1 user1 person1@example.com",
      "select",
      ".exit",
    ])
    expect(result).to match_array([
      "db > Executed.",
      "db > (1, user1, person1@example.com)",
      "Executed.",
      "db > ",
    ])
  end

  it 'prints constants' do
    result = run_script([".constants", ".exit"])
    expect(result).to match_array([
      "db > Constants:",
      "ROW_SIZE: 291",
      "COMMON_NODE_HEADER_SIZE: 6",
      "LEAF_NODE_HEADER_SIZE: 14",
      "LEAF_NODE_CELL_SIZE: 295",
      "LEAF_NODE_SPACE_FOR_CELLS: 4082",
      "LEAF_NODE_MAX_CELLS: 13",
      "db > ",
    ])
  end

  it 'allows printing out the structure of a one-node btree' do
    script = [3, 1, 2].map { |i| "insert #{i} user#{i} person#{i}@example.com" }
    script << ".btree"
    script << ".exit"
    result = run_script(script)
    expect(result).to match_array([
      "db > Executed.",
      "db > Executed.",
      "db > Executed.",
      "db > Tree:",
      "- leaf (size 3)",
      "  - 1",
      "  - 2",
      "  - 3",
      "db > ",
    ])
  end

  it 'allows inserting strings that are the maximum length' do
    long_username = "a" * 32
    long_email    = "a" * 255
    result = run_script([
      "insert 1 #{long_username} #{long_email}",
      "select",
      ".exit",
    ])
    expect(result).to match_array([
      "db > Executed.",
      "db > (1, #{long_username}, #{long_email})",
      "Executed.",
      "db > ",
    ])
  end

  it 'prints error message if strings are too long' do
    long_username = "a" * 33
    long_email    = "a" * 256
    result = run_script([
      "insert 1 #{long_username} #{long_email}",
      "select",
      ".exit",
    ])
    expect(result).to match_array([
      "db > String is too long.",
      "db > Executed.",
      "db > ",
    ])
  end

  it 'prints an error message if id is negative' do
    result = run_script([
      "insert -1 cstack foo@bar.com",
      "select",
      ".exit",
    ])
    expect(result).to match_array([
      "db > ID must be positive.",
      "db > Executed.",
      "db > ",
    ])
  end

  it 'keeps data after closing connection' do
    result1 = run_script([
      "insert 1 user1 person1@example.com",
      ".exit",
    ])
    expect(result1).to match_array([
      "db > Executed.",
      "db > ",
    ])

    result2 = run_script([
      "select",
      ".exit",
    ])
    expect(result2).to match_array([
      "db > (1, user1, person1@example.com)",
      "Executed.",
      "db > ",
    ])
  end

  it 'allows printing structure of a 3-leaf-node btree' do
    script = (1..14).map { |i| "insert #{i} user#{i} person#{i}@example.com" }
    script << ".btree"
    script << ".exit"
    result = run_script(script)

    expect(result[14]).to eq("db > Tree:")
    expect(result[15]).to eq("- internal (size 1)")
    expect(result[16]).to eq("  - leaf (size 7)")
    (1..7).each do |i|
      expect(result[16 + i]).to eq("    - #{i}")
    end
    expect(result[24]).to eq("  - key 7")
    expect(result[25]).to eq("  - leaf (size 7)")
    (8..14).each do |i|
      expect(result[25 + (i - 7)]).to eq("    - #{i}")
    end
  end

  it 'allows inserting more than 14 rows (non-root leaf split)' do
    script = (1..21).map { |i| "insert #{i} user#{i} person#{i}@example.com" }
    script << ".btree"
    script << ".exit"
    result = run_script(script)

    expect(result[21]).to eq("db > Tree:")
    expect(result[22]).to eq("- internal (size 2)")
    expect(result[23]).to eq("  - leaf (size 7)")
    (1..7).each { |i| expect(result[23 + i]).to eq("    - #{i}") }
    expect(result[31]).to eq("  - key 7")
    expect(result[32]).to eq("  - leaf (size 7)")
    (8..14).each { |i| expect(result[32 + (i - 7)]).to eq("    - #{i}") }
    expect(result[40]).to eq("  - key 14")
    expect(result[41]).to eq("  - leaf (size 7)")
    (15..21).each { |i| expect(result[41 + (i - 14)]).to eq("    - #{i}") }
  end

  it 'allows inserting 35 rows (internal node split)' do
    script = (1..35).map { |i| "insert #{i} user#{i} person#{i}@example.com" }
    script << ".btree"
    script << ".exit"
    result = run_script(script)
    expect(result[35]).to eq("db > Tree:")
    expect(result[36]).to eq("- internal (size 1)")
    expect(result[37]).to eq("  - internal (size 2)")
    expect(result[38]).to eq("    - leaf (size 7)")
    (1..7).each   { |i| expect(result[38 + i]).to eq("      - #{i}") }
    expect(result[46]).to eq("    - key 7")
    expect(result[47]).to eq("    - leaf (size 7)")
    (8..14).each  { |i| expect(result[47 + (i - 7)]).to eq("      - #{i}") }
    expect(result[55]).to eq("    - key 14")
    expect(result[56]).to eq("    - leaf (size 7)")
    (15..21).each { |i| expect(result[56 + (i - 14)]).to eq("      - #{i}") }
    expect(result[64]).to eq("  - key 21")
    expect(result[65]).to eq("  - internal (size 1)")
    expect(result[66]).to eq("    - leaf (size 7)")
    (22..28).each { |i| expect(result[66 + (i - 21)]).to eq("      - #{i}") }
    expect(result[74]).to eq("    - key 28")
    expect(result[75]).to eq("    - leaf (size 7)")
    (29..35).each { |i| expect(result[75 + (i - 28)]).to eq("      - #{i}") }
  end

  it 'deletes a row and it no longer appears in select' do
    result = run_script([
      "insert 1 user1 person1@example.com",
      "insert 2 user2 person2@example.com",
      "insert 3 user3 person3@example.com",
      "delete 2",
      "select",
      ".exit",
    ])
    expect(result).to match_array([
      "db > Executed.",
      "db > Executed.",
      "db > Executed.",
      "db > Executed.",
      "db > (1, user1, person1@example.com)",
      "(3, user3, person3@example.com)",
      "Executed.",
      "db > ",
    ])
  end

  it 'deleting the max key updates the parent separator' do
    # Insert 14 rows → 2-leaf tree.  Leaf 1 holds 1-7 (separator key = 7).
    # Delete row 7 → leaf 1's new max is 6 → separator in parent must become 6.
    script = (1..14).map { |i| "insert #{i} user#{i} person#{i}@example.com" }
    script << "delete 7"
    script << ".btree"
    script << ".exit"
    result = run_script(script)

    # After deletion the separator should read 6, not 7
    expect(result).to include("  - key 6")
    expect(result).not_to include("  - key 7")
  end

  it 'returns an error when deleting a non-existent key' do
    result = run_script([
      "insert 1 user1 person1@example.com",
      "delete 99",
      "select",
      ".exit",
    ])
    expect(result).to match_array([
      "db > Executed.",
      "db > Error: Key not found.",
      "db > (1, user1, person1@example.com)",
      "Executed.",
      "db > ",
    ])
  end

  it 'persists deletions across connections' do
    run_script([
      "insert 1 user1 person1@example.com",
      "insert 2 user2 person2@example.com",
      "delete 1",
      ".exit",
    ])
    result = run_script(["select", ".exit"])
    expect(result).to match_array([
      "db > (2, user2, person2@example.com)",
      "Executed.",
      "db > ",
    ])
  end

  it 'updates a row and the new values appear in select' do
    result = run_script([
      "insert 1 alice alice@example.com",
      "insert 2 bob bob@example.com",
      "update 1 alicia alicia@example.com",
      "select",
      ".exit",
    ])
    expect(result).to match_array([
      "db > Executed.",
      "db > Executed.",
      "db > Executed.",
      "db > (1, alicia, alicia@example.com)",
      "(2, bob, bob@example.com)",
      "Executed.",
      "db > ",
    ])
  end

  it 'returns an error when updating a non-existent key' do
    result = run_script([
      "insert 1 alice alice@example.com",
      "update 99 ghost ghost@example.com",
      "select",
      ".exit",
    ])
    expect(result).to match_array([
      "db > Executed.",
      "db > Error: Key not found.",
      "db > (1, alice, alice@example.com)",
      "Executed.",
      "db > ",
    ])
  end

  it 'persists updates across connections' do
    run_script([
      "insert 1 alice alice@example.com",
      "update 1 alicia alicia@example.com",
      ".exit",
    ])
    result = run_script(["select", ".exit"])
    expect(result).to match_array([
      "db > (1, alicia, alicia@example.com)",
      "Executed.",
      "db > ",
    ])
  end

  it 'prints an error message when inserting a duplicate id' do
    result = run_script([
      "insert 1 user1 person1@example.com",
      "insert 1 user1 person1@example.com",
      "select",
      ".exit",
    ])
    expect(result).to match_array([
      "db > Executed.",
      "db > Error: Duplicate key.",
      "db > (1, user1, person1@example.com)",
      "Executed.",
      "db > ",
    ])
  end
end
