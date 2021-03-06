-- This file is protected by Copyright. Please refer to the COPYRIGHT file
-- distributed with this source distribution.
--
-- This file is part of OpenCPI <http://www.opencpi.org>
--
-- OpenCPI is free software: you can redistribute it and/or modify it under the
-- terms of the GNU Lesser General Public License as published by the Free
-- Software Foundation, either version 3 of the License, or (at your option) any
-- later version.
--
-- OpenCPI is distributed in the hope that it will be useful, but WITHOUT ANY
-- WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
-- A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
-- details.
--
-- You should have received a copy of the GNU Lesser General Public License
-- along with this program. If not, see <http://www.gnu.org/licenses/>.


-------------------------------------------------------------------------------
-- Capture_v2
-------------------------------------------------------------------------------
--
-- Description:
--
-- The capture_v2 worker provides the ability to store an input port's
-- data. Two modes are supported:
-- * 'leading' - capture all messages from input port until the buffer is full.
-- This has the affect of capturing messages from the start of an application.
-- * 'trailing' - capture all messages from input port and allow the buffer's
-- content to be overwritten while application is in operation. This has the affect
-- of capturing all messages (a buffers worth) near the end of an application.
--
-- This worker provides an optional output port, so that, it may be
-- placed between two workers. The input messages are directly passed to
-- the output port with a latency of the control plane clock cycles.
--
-- The capture_v2 worker takes input port messages and stores their data in a
-- buffer via the data property as 4 byte words. Metadata associated with each
-- message is stored as a record in a metadata buffer via the metadata property.
-- It captures four 4 byte words of metadata. The first metadata word is the
-- opcode of the message and message size (bytes); opcode 8 MSB and message
-- size 24 LSB. The second word is the fraction time stamp for the EOM. The
-- third word is the fraction time stamp for the SOM. And the fourth word is
-- the seconds timestamp for the SOM. So that the metadata can be read on a
-- little-endian processor, the ordering of the metadata is as follows:
-- 1) opcode (8-bit) & message size (24-bit)
-- 2) eom fraction (32-bit)
-- 3) som fraction (32-bit)
-- 4) som seconds (32-bit)
--
-- When the number of bytes sent for a message is not a multiple of 4, only the
-- last (number of bytes modulus 4) least significant bytes of the last word
-- in the data buffer represent received data. The (number of bytes modulus 4)
-- most significant bytes will always be zero in the last word in the data
-- buffer.
-- For example, given that last captured data buffer word is 0x0000005a:
-- - if number of bytes is 5, the last data received was 0x5a.
-- - if number of bytes is 6, the last data received was 0x005a.
--
-- The capture_v2 worker counts the number of metadata records (metadataCount)
-- have been captured and how many data words have been captured (dataCount).
-- It allows for the option to wrap around and continue to capture data and
-- metadata once the buffers are full or to stop capturing data and metadata
-- when the data and metadata buffers are full via the stoponFull property.
--
-- When stopOnFull is true (leading), data and metadata will be captured as long as the
-- metadata buffer is not full. If the data buffer is full before
-- the metadata buffer, metadata will still be captured until the metadata buffer
-- is full. If the metadata buffer is full before the data buffer, no more metadata
-- and no more data will be captured.
-- When stopOnFull is false (trailing), there will be a wrap around when the data
-- and metadata buffers are full and data metadata will continue to be captured.
--
-- The worker also has properties that keep track of whether or not the
-- metadata and data buffers are full; metaFull and dataFull.
--
-- Something to note is that the BRAM2 module that is used to store the data
-- and metadata initializes the BRAM to an initial value of 0xAAAAAAAA
-- (2863311530 in base 10) in simulation. This means at the start of an application,
-- in simulation, the data and metadata properties will have an initial value of
-- 0xAAAAAAAA.
--

-------------------------------------------------------------------------------
-- TODO - Add a "trigger" capability that will allow the capture of data based on
-- on a condition, i.e, "start capturing when a second zlm has occurred or
-- start capturing when an opcode of 5 has been seen"
-------------------------------------------------------------------------------

library IEEE; use IEEE.std_logic_1164.all; use ieee.numeric_std.all;
library ocpi; use ocpi.util.all; use ocpi.types.all; -- remove this to avoid all ocpi name collisions
library util; use util.util.all;

architecture rtl of capture_v2_worker is

-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
-- Signals for capture logic
-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
signal stopOnFull       : boolean;
signal metadataCount    : ulong_t := (others => '0');
signal dataCount        : ulong_t := (others => '0');
signal metaFull         : boolean := false;
signal dataFull         : boolean := false;
signal bytes            : integer := 0; -- Number of bytes decoded from byte enable
signal valid_bytes      : std_logic_vector(31 downto 0) := (others => '0'); -- Valid bytes determined from byte enable
signal messageSize      : unsigned(23 downto 0) := (others => '0'); -- Total number of bytes sent during a message
signal s_som_seconds      : std_logic_vector(31 downto 0) := (others => '0'); -- Seconds time stamp captured for som
signal s_som_fraction     : std_logic_vector(31 downto 0) := (others => '0'); -- Fraction time stamp captured for som
signal s_in_seconds_r   : std_logic_vector(31 downto 0) := (others => '0'); -- Latched value of seconds time stamp captured for som
signal s_in_fraction_r  : std_logic_vector(31 downto 0) := (others => '0'); -- Latched value of fraction time stamp captured for som
signal finished         : std_logic := '0';   -- Used to drive ctl_out.finished
-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
-- Signals for combinatorial logic
-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
signal dataDisable       : std_logic := '0'; -- When stopOnFull is true, used to disable writing to data BRAM when data is full
signal metadataDisable   : std_logic := '0'; -- When stopOnFull is true, used to disable writing to metadata BRAM and data BRAM when metadata is full
signal eom               : std_logic := '0'; -- Used for counting eoms
signal som               : std_logic := '0'; -- Used for counting soms
signal zlm               : std_logic := '0'; -- Used for zlms
signal s_is_read_r       : std_logic := '0'; -- Register to aid in incrementing the BRAM read side addresses
signal s_in_eom_r        : std_logic := '0'; -- Register to aid in detecting multi clock cycle eom. Used for combinatorial logic to drive eom signal
signal s_in_som_r        : std_logic := '0'; -- Register to aid in detecting multi clock cycle som. Used for combinatorial logic to drive som signal
signal s_take_r          : std_logic := '0'; -- Register used for combinatorial logic to drive eom and som signals
signal s_in_ready_r      : std_logic := '0'; -- Register used for combinatorial logic to drive capture_vld signal
signal capture_vld       : std_logic := '0'; -- Used to determine if data being captured is valid
signal outNotConnected   : std_logic := '0';
signal take              : std_logic := '0';
signal give              : std_logic := '0';
-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
-- Data BRAM constants and signals
-- Writing to the BRAM B side and then reading from the BRAM A side
-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
constant data_memory_depth_c  : natural := to_integer(numDataWords);
constant data_addr_width_c    : natural := width_for_max(data_memory_depth_c - 1);
constant data_bram_dsize_c    : natural := 32; -- Size in bytes of the data for data bram
signal data_bramW_in          : std_logic_vector(data_bram_dsize_c-1 downto 0) := (others => '0');
signal data_bramR_addr        : unsigned(data_addr_width_c-1 downto 0) := (others => '0');
signal data_bramW_write       : std_logic := '0';
signal data_bramR_out         : std_logic_vector(data_bram_dsize_c-1 downto 0) := (others => '0');
signal data_bramW_addr        : unsigned(data_addr_width_c-1 downto 0) := (others => '0');
-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
-- Metadata BRAM constants and signals
-- Writing to the BRAM B side and then reading from the BRAM A side
-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
constant metadata_memory_depth_c  : natural := to_integer(numRecords);
constant metadata_addr_width_c    : natural := width_for_max(metadata_memory_depth_c - 1);
constant metadata_bram_dsize_c    : natural := 128; -- Size in bytes of the data for metadata bram
signal metadata_bramW_in          : std_logic_vector(metadata_bram_dsize_c-1 downto 0) := (others => '0');
signal metadata_bramR_out         : std_logic_vector(127 downto 0) := (others => '0');
alias metadata_word1              : std_logic_vector(127 downto 96) is metadata_bramR_out(127 downto 96);
alias metadata_word2              : std_logic_vector(95 downto 64) is metadata_bramR_out(95 downto 64);
alias metadata_word3              : std_logic_vector(63 downto 32) is metadata_bramR_out(63 downto 32);
alias metadata_word4              : std_logic_vector(31 downto 0) is metadata_bramR_out(31 downto 0);
signal metadata_bramW_write       : std_logic := '0';
signal metadata_bramR_addr        : unsigned(metadata_addr_width_c-1 downto 0) := (others => '0');
signal metadata_bramW_addr        : unsigned(metadata_addr_width_c-1 downto 0) := (others => '0');
signal metadata                   : std_logic_vector(31 downto 0) := (others => '0'); -- Used to store metadata words from metadata_bramR_out after doing some decoding
constant counter_size             : natural := width_for_max(3); -- Size in bytes for metadata_bramR_ctr
signal metadata_bramR_ctr         : unsigned(counter_size-1 downto 0) := (others => '0'); -- Used for the metadata decoding
-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
-- Mandatory input port logic
-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
signal ready_for_in_port_data  : std_logic := '0';
-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
-- Mandatory output port logic
-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
signal data_ready_for_out_port : std_logic := '0';
signal out_meta_is_reserved    : std_logic := '0';
signal out_som                 : std_logic := '0';
signal out_eom                 : std_logic := '0';
signal out_valid               : std_logic := '0';

begin

-- Mandatory input port logic
-- (note that ready_for_in_port_data MUST have the same clock latency as the latency
-- between in port's data and any associated output port's data)
in_out.take <= take;

take <= ctl_in.is_operating and in_in.ready and
              ready_for_in_port_data; -- this applies backpressure

-- Mandatory output port logic
-- (note that data_ready_for_out_port MUST be clock-aligned with out_out.data)
-- (note that reserved messages will be DROPPED ON THE FLOOR)
out_out.give <= give;

give <= ctl_in.is_operating and out_in.ready and
              (not out_meta_is_reserved) and data_ready_for_out_port;

out_meta_is_reserved <= (not out_som) and (not out_valid) and (not out_eom);
out_out.som   <= out_som;
out_out.eom   <= out_eom;
out_out.valid <= out_valid;

out_som <= in_in.som;
out_eom <= in_in.eom;
out_valid <= in_in.valid;


ready_for_in_port_data <= out_in.ready or outNotConnected;
data_ready_for_out_port <= in_in.ready;

-- When an optional output port is not connected, out_in.reset will always be '1'
outNotConnected <= out_in.reset;

out_out.data <= in_in.data;
out_out.byte_enable <= in_in.byte_enable;
out_out.opcode   <= in_in.opcode;

props_out.metadataCount <= metadataCount;
props_out.dataCount <= dataCount;
props_out.metaFull <= to_bool(metaFull);
props_out.dataFull <= to_bool(dataFull);

capture_vld <= ctl_in.is_operating and in_in.valid and
               ((in_in.ready and (not s_in_ready_r)) or
                (in_in.ready and s_take_r));

stopOnFull <= to_boolean(props_in.stopOnFull);


eom <= in_in.eom and in_in.ready and (not s_in_eom_r or s_take_r);
som <= in_in.som and in_in.ready and (not s_in_som_r or s_take_r);


-- If a single cycle single word message or ZLM occurs,
-- use the non-latched seconds/fraction timestamp,
-- otherwise use the latched version of the timestamp
s_som_seconds <= std_logic_vector(time_in.seconds) when (its((som and eom))) else
               s_in_seconds_r;
s_som_fraction <= std_logic_vector(time_in.fraction) when (its((som and eom))) else
                s_in_fraction_r;

ctl_out.finished  <= finished;

props_out.raw.done  <= '1';
props_out.raw.error <= '0';

data_bramW_in <= std_logic_vector(in_in.data and valid_bytes);

-- Write to data BRAM when data is valid, when metadata and
-- data not full (when stopOnFull = true).
data_bramW_write <= capture_vld and (not dataDisable) and (not metadataDisable) and (not finished);

-- Metadata to place into metadata BRAM
metadata_bramW_in <= in_in.opcode &
                     std_logic_vector(messageSize+bytes) &
                     std_logic_vector(time_in.fraction) & s_som_fraction & s_som_seconds;

-- Write to metadata BRAM when there is an eom. And if stopOnFull is true write when metadata not full.
metadata_bramW_write <= eom and (not metadataDisable) and (not finished);


-- Since metadata is the first raw property, assign raw.data to metadata until
-- finished reading out metadata and then assign raw.data to the data.
-- Divide the raw.address by 4 because raw.address increments in steps of 4.
-- Mulitplying numRecords by 4 because there are 4 metadata words.
props_out.raw.data <= metadata when (to_integer(props_in.raw.address)/4 < 4*numRecords) else data_bramR_out;

-- Decode to get each metadata word
metadata <= metadata_word1 when (metadata_bramR_ctr = 0) else
            metadata_word2 when (metadata_bramR_ctr = 1) else
            metadata_word3 when (metadata_bramR_ctr = 2) else
            metadata_word4 when (metadata_bramR_ctr = 3);


-- What each byte_enable value means in number of bytes
bytes <= 4 when (in_in.byte_enable(3) = '1' and in_in.valid = '1') else
         3 when (in_in.byte_enable(2) = '1' and in_in.valid = '1') else
         2 when (in_in.byte_enable(1) = '1' and in_in.valid = '1') else
         1 when (in_in.byte_enable(0) = '1' and in_in.valid = '1') else
         0;

valid_bytes(31 downto 24) <= (others => in_in.byte_enable(3));
valid_bytes(23 downto 16) <= (others => in_in.byte_enable(2));
valid_bytes(15 downto  8) <= (others => in_in.byte_enable(1));
valid_bytes(7 downto  0) <= (others => in_in.byte_enable(0));


-- Use zlm detector primitive to detect zlms and multicycle zlms
zlm_detector : util.util.zlm_detector
  port map (
    clk => ctl_in.clk,
    reset => ctl_in.reset,
    som => in_in.som,
    valid => in_in.valid,
    eom => in_in.eom,
    ready => in_in.ready,
    take => take,
    eozlm_pulse => open,
    eozlm => zlm);

dataBram : component util.util.BRAM2
generic map(PIPELINED  => 0,
            ADDR_WIDTH => data_addr_width_c,
            DATA_WIDTH => data_bram_dsize_c,
            MEMSIZE    => data_memory_depth_c)
  port map   (CLKA       => ctl_in.clk,
              ENA        => '1',
              WEA        => '0',
              ADDRA      => std_logic_vector(data_bramR_addr),
              DIA        => x"00000000",
              DOA        => data_bramR_out,
              CLKB       => ctl_in.clk,
              ENB        => '1',
              WEB        => data_bramW_write,
              ADDRB      => std_logic_vector(data_bramW_addr),
              DIB        => data_bramW_in,
              DOB        => open);

  metadataBram : component util.util.BRAM2
  generic map(PIPELINED  => 0,
              ADDR_WIDTH => metadata_addr_width_c,
              DATA_WIDTH => metadata_bram_dsize_c,
              MEMSIZE    => metadata_memory_depth_c)
    port map   (CLKA       => ctl_in.clk,
                ENA        => '1',
                WEA        => '0',
                ADDRA      => std_logic_vector(metadata_bramR_addr),
                DIA        => x"00000000000000000000000000000000",
                DOA        => metadata_bramR_out,
                CLKB       => ctl_in.clk,
                ENB        => '1',
                WEB        => metadata_bramW_write,
                ADDRB      => std_logic_vector(metadata_bramW_addr),
                DIB        => metadata_bramW_in,
                DOB        => open);

-- Running counter to keep track of data BRAM reading side address
-- Increments counter when props_in.raw.address/4 >= 4*numRecords because
-- the metadata property is stored in the addresses prior to that
data_bramR_addr_counter : process (ctl_in.clk)
begin
  if rising_edge(ctl_in.clk) then
    if ctl_in.reset = '1' then
      data_bramR_addr  <= (others => '0');
    elsif ((to_integer(props_in.raw.address)/4 >= 4*numRecords) and props_in.raw.is_read = '1' and s_is_read_r = '0') then
      if (data_bramR_addr = numDataWords-1) then
        data_bramR_addr <= (others => '0');
      else
        data_bramR_addr <= data_bramR_addr + 1;
      end if;
    end if;
  end if;
end process data_bramR_addr_counter;


-- Counter used to decode metadata and to advance metadata_bramR_addr
-- Increments counter when props_in.raw.address/4 < 4*numRecords because
-- the metadata property is the first raw property
metadata_bramR_counter : process (ctl_in.clk)
begin
  if rising_edge(ctl_in.clk) then
    if ctl_in.reset = '1' then
      metadata_bramR_ctr <= (others => '0');
    -- Using s_is_read_r = '1' because the metadata signal logic relies on metadata_bramR_ctr
    -- and metadata signal is driven combinatorially. is_read is on high for 2 clock cycles and since metadata signal is driven combinatorially, metadata_bramR_ctr
    -- has to stay the same value until is_read goes low so that the metadata signal's value doesn't change until after is_read goes low.
    elsif ((to_integer(props_in.raw.address)/4 < 4*numRecords) and props_in.raw.is_read = '1' and s_is_read_r = '1') then
          metadata_bramR_ctr <= metadata_bramR_ctr + 1;
    end if;
  end if;
end process metadata_bramR_counter;

-- Running counter to keep track of metadata BRAM reading side address
-- Increments counter when props_in.raw.address/4 < 4*numRecords because
-- the metadata property is the first raw property
metadata_bramR_addr_counter : process (ctl_in.clk)
begin
  if rising_edge(ctl_in.clk) then
    if ctl_in.reset = '1' then
      metadata_bramR_addr  <= (others => '0');
    -- Increment metadata_bramR_addr only when ready to read in metadata
    elsif ((to_integer(props_in.raw.address)/4 < 4*numRecords) and props_in.raw.is_read = '1' and s_is_read_r = '0') then
    -- Reset metadata_bramR_addr to 0 when on the last metadata word
        if (metadata_bramR_addr = numRecords-1 and metadata_bramR_ctr = 3) then
          metadata_bramR_addr <= (others => '0');
        elsif (metadata_bramR_ctr = 3) then
          metadata_bramR_addr <= metadata_bramR_addr + 1;
        end if;
    end if;
  end if;
end process metadata_bramR_addr_counter;

-- Latch the timestamp at start of message
som_timestamp_reg : process(ctl_in.clk)
begin
  if rising_edge(ctl_in.clk) then
    if ctl_in.reset = '1' then
      s_in_seconds_r <= (others => '0');
      s_in_fraction_r <= (others => '0');
    elsif (som = '1') then
      s_in_seconds_r <= std_logic_vector(time_in.seconds);
      s_in_fraction_r <= std_logic_vector(time_in.fraction);
    end if;
  end if;
end process som_timestamp_reg;

set_regs : process (ctl_in.clk)
begin
    if rising_edge(ctl_in.clk) then
      if ctl_in.reset = '1' then
        s_is_read_r <= '0';
        s_in_eom_r <= '0';
        s_in_som_r <= '0';
        s_take_r <= '0';
        s_in_ready_r <= '0';
      else
        s_is_read_r <= props_in.raw.is_read;
        s_in_eom_r <= in_in.eom and in_in.ready;
        s_in_som_r <= in_in.som and in_in.ready;
        s_take_r <= take;
        s_in_ready_r <= in_in.ready;
      end if;
    end if;
end process set_regs;


-- Counts the total number of bytes for a message
messageSize_counter : process (ctl_in.clk)
begin
  if rising_edge(ctl_in.clk) then
    if (ctl_in.reset = '1' or eom = '1') then
      messageSize <= (others =>'0');
    elsif (capture_vld = '1' and finished = '0') then
        messageSize <= messageSize + bytes;
    end if;
  end if;
end process messageSize_counter;

-- Take data while buffer is in wrapping mode (stopOnFull=false) or
-- single capture (stopOnFull=true) and data and metadata counts
-- have not reached their respective maximum.
data_capture : process (ctl_in.clk)
begin
    if rising_edge(ctl_in.clk) then
      if ctl_in.reset = '1' then
        dataCount <= (others => '0');
        data_bramW_addr <= (others => '0');
        dataDisable <= '0';
      elsif (capture_vld = '1' and finished = '0') then

          if ((stopOnFull = true and (not (dataCount = numDataWords) and not (metadataCount = numRecords))) or stopOnFull = false) then
            dataCount <= dataCount + 1;
          end if;

          -- Configured for a single buffer capture
          if (stoponFull = true and not (dataCount = numDataWords) and not (metadataCount = numRecords)) then
            -- Disable writing to data BRAM when metadata is full
            if (dataCount = numDataWords-1) then
              dataDisable <= '1';
            end if;
            data_bramW_addr <= data_bramW_addr + 1;
          else -- Configured for wrap-around buffer capture
            if (data_bramW_addr = numDataWords -1) then
              data_bramW_addr <= (others => '0');
            else
              data_bramW_addr <= data_bramW_addr + 1;
            end if;
          end if;
        end if;
    end if;
  end process data_capture;

  -- Store metadata after each message or if stopOnFull is true store metadata until data full or metadata full
  metadata_capture : process (ctl_in.clk)
  begin
    if rising_edge(ctl_in.clk) then
      if ctl_in.reset = '1' then
        metadataCount <= (others => '0');
        metadata_bramW_addr <= (others => '0');
        metadataDisable <= '0';
      elsif (eom = '1' and finished = '0') then

           if ((stopOnFull = true and not (metadataCount = numRecords)) or stopOnFull = false) then
             metadataCount <= metadataCount + 1;
           end if;

           -- Configured for a single buffer capture
           if (stopOnFull = true and not (metadataCount = numRecords)) then
             -- Disable writing to metadata and data BRAMs when metadata is full
            if (metadataCount = numRecords-1) then
              metadataDisable <= '1';
            end if;
            metadata_bramW_addr <= metadata_bramW_addr + 1;
           else -- Configured for wrap-around buffer capture
            if (metadata_bramW_addr = numRecords-1)  then
              metadata_bramW_addr <= (others => '0');
            else
              metadata_bramW_addr <= metadata_bramW_addr + 1;
            end if;
         end if;
       end if;
     end if;
  end process metadata_capture;


  -- Sticky-bit full flags for data and metadata buffers
  buffer_full : process (ctl_in.clk)
  begin
    if rising_edge(ctl_in.clk) then
      if ctl_in.reset = '1' then
        dataFull <= false;
        metaFull <=false;
      else
        if (dataCount = numDataWords) then
          dataFull <= true;
        end if;
        if (metadataCount = numRecords) then
          metaFull <= true;
        end if;
      end if;
    end if;
  end process buffer_full;

-- Determines when the worker is finished.
-- For stopOnEOF, if there is a ZLM and has been GIVEN (if output connected),
-- the input opcode is equal to 0, and stopOnEOF is true then set finished to true.
-- For stopOnZLM, if there is a ZLM and has been GIVEN (if output connected),
-- the input opcode is equal to stopZLMOpcode, and stopOnZLM is true then set finished to true.
finish : process (ctl_in.clk)
  begin
      if rising_edge(ctl_in.clk) then
        if ctl_in.reset = '1' then
          finished <= '0';
          elsif (zlm = '1' and (give = '1' or outNotConnected = '1') and
                 ((props_in.stopOnEOF and unsigned(in_in.opcode) = 0) or
                  (to_uchar(in_in.opcode) = props_in.stopZLMOpcode and props_in.stopOnZLM))) then
            finished <= '1';
        end if;
      end if;
end process finish;

end rtl;
