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

-- Lime DAC TimeStamped worker
library IEEE, ocpi;
use IEEE.std_logic_1164.all, IEEE.numeric_std.all, ocpi.types.all, ocpi.wci.all, ocpi.util.all;
library util; use util.util.all;
architecture rtl of lime_dac_ts_worker is
  constant dac_width          : positive := 12; -- must not be > 15 due to WSI width
  -- FIFO parameters
  constant FIFO_WIDTH_c       : natural := 26; -- the fifo is just wide enough to feed lime DAC plus EOM flag plus Flush flag
  constant FIFO_DEPTH_c       : natural := to_integer(unsigned(FIFO_DEPTH_p));
  constant SCALE_FACTOR_c     : real    := 2.0**to_integer(unsigned(FRACTION_WIDTH_p));
  constant SECONDS_WIDTH_c    : natural := to_integer(unsigned(SECONDS_WIDTH_p));
  constant FRACTION_WIDTH_c   : natural := to_integer(unsigned(FRACTION_WIDTH_p));
  -- CTL/WSI Clock domain signals
  signal in_I                 : std_logic_vector(15 downto 0);
  signal in_Q                 : std_logic_vector(15 downto 0);
  signal wsi_data_I           : std_logic_vector(dac_width- 1 downto 0);
  signal wsi_data_Q           : std_logic_vector(dac_width- 1 downto 0);
  signal wsi_data             : std_logic_vector(FIFO_WIDTH_c - 1 downto 0);
  signal wsi_valid            : std_logic;
  signal wsi_take             : std_logic; -- take data
  signal ts_take              : std_logic; -- take timestamp and other non-data
  signal nvalid_take          : std_logic; -- take when not valid
  signal flush_written        : std_logic;
  signal tx_time_int          : unsigned(time_in.seconds'length - 1 downto 0);
  signal tx_time_frac         : unsigned(time_in.fraction'length - 1 downto 0);
  signal prev_opcode          : TimeStamped_IQ_OpCode_t;
  -- Lime TX clock domain signals
  signal dac_reset            : bool_t;
  signal dac_clk              : std_logic; -- clock sent to lime tx (if connected)
  signal dac_data             : std_logic_vector(FIFO_WIDTH_c - 1 downto 0);
  signal dac_data_r           : std_logic_vector(FIFO_WIDTH_c/2 - 2 downto 0);
  signal dac_eom              : std_logic;
  signal dac_eom_r1           : std_logic;
  signal dac_eom_r2           : std_logic;
  signal dac_ready            : bool_t;    -- fifo has data for DAC
  signal dac_ready_r1         : bool_t;
  signal dac_take             : bool_t;    -- take data from FIFO
  signal dac_flush            : std_logic;
  signal dac_flush_hold       : std_logic;
  signal dac_flush_event      : std_logic;
  signal flush_op             : std_logic;
  signal flush_flag           : std_logic;
  signal flush_count          : integer range 0 to to_integer(unsigned(FLUSH_DEPTH_p))-1;
  signal sel_iq_r             : bool_t;    -- true for I, false for Q, sent to lime
  signal cont_mode            : std_logic;
  signal cont_mode_dclk       : std_logic;
  signal new_ts_flag          : std_logic;
  signal new_ts_flag_dclk     : std_logic;
  signal transmit_now         : std_logic;
  signal transmit_now_r1      : std_logic;
  signal transmit_now_re      : std_logic := '0';
  signal transmit_busy        : std_logic;
  signal transmit_now_dclk    : std_logic;
  signal transmit_now_dclk_r1 : std_logic;
  signal transmit_now_dclk_r2 : std_logic;
  signal tx_en_d              : std_logic;
  signal tx_en_d_cclk         : std_logic;
  signal tx_en_d_cclk_r1      : std_logic;
  signal tx_en_d_cclk_re      : std_logic;
  signal tx_en_d_cclk_fe      : std_logic;
  signal doit                 : std_logic;
  -- Event Port Signals
  signal event_port_connected         : std_logic := '0';
  signal event_pending                : std_logic := '0';
  signal wsi_event_in_opcode_on_off   : std_logic := '0';
  signal event_in_out_take            : std_logic := '0';
  signal ready_for_event_in_port_data : std_logic := '0';
  signal done_flag                    : std_logic := '0';
  signal event_in_clk                 : std_logic := '0';
  signal event_in_reset               : std_logic := '0';
  signal worker_driven_txen           : std_logic := '0';
  signal event_driven_txen            : std_logic := '0';
begin
  --TODO: Replace ctl clock with port clock once AV-65 is resolved 
  event_in_clk <= ctl_in.clk;
  event_in_reset <= ctl_in.reset;
  --------------------------------------------------------------------------------
  -- DAC Sample Clock choices
  --------------------------------------------------------------------------------
  -- 1. We get a copy of what is wired to the lime's TX_CLK input, and use that.
  -- 2. We get a clock that *we* are supposed to drive to the lime TX_CLK, and use that.
  -- 4. We divide-down the control clock, use it, and drive TX_CLK
  -- 5. We use a container clock, use that, and drive TX_CLK
  dac_clk <= TX_CLK_IN when its(USE_CLK_IN_p) else
             ctl_in.clk when its(USE_CTL_CLK_p) else
             sample_clk;

  tx_clk_gen : if its(DRIVE_CLK_p) generate
    clk_fwd_gen : clock_forward
      generic map (
        INVERT_CLOCK => its(TX_CLK_INV_p)
      )
      port map (
        RST       => dac_reset,
        CLK_IN    => dac_clk,
        CLK_OUT_P => TX_CLK,
        CLK_OUT_N => open
      );
  end generate tx_clk_gen;

  no_tx_clk_gen : if not(its(DRIVE_CLK_p)) generate
    TX_CLK <= '0';
  end generate no_tx_clk_gen;

  -- Output/lime clock domain processing.
  -- Mux I and Q onto TXD, consistent with the TX_IQ_SEL output to lime
  output_proc: process(dac_clk)
  begin
    if rising_edge(dac_clk) then
      if (dac_reset = '1') then
        sel_iq_r     <= '0';
        dac_eom_r2   <= '0';
        dac_eom_r1   <= '0';
        dac_ready_r1 <= '0';
        TX_IQ_SEL    <= '0';
        TXD          <= (others => '0');
        dac_data_r   <= (others => '0');
      else
        sel_iq_r     <= not sel_iq_r;
        dac_eom_r2   <= dac_eom_r1;
        dac_eom_r1   <= dac_eom;
        dac_ready_r1 <= dac_ready;
        TX_IQ_SEL    <= sel_iq_r;
        if (flush_flag = '1' and sel_iq_r = '1') then
          TXD        <= (others => '0');
        elsif (tx_en_d = '1' and flush_count = 0 and dac_flush = '0') then
          TXD        <= dac_data_r;
        else
          TXD        <= (others => '0');
        end if;
        if (dac_flush = '1') then
          dac_data_r <= (others => '0');
        else
          if (its(sel_iq_r)) then
            dac_data_r <= dac_data(23 downto 12);
          else
            dac_data_r <= dac_data(11 downto 0);
          end if;
        end if;
      end if;
    end if;
  end process output_proc;

  flush_written <= props_in.flush_written and ctl_in.is_operating;

  flush_written_cdc : flag_cross_domain
    port map (
      clkA         => ctl_in.clk,
      flagIn_clkA  => flush_written,
      busy_clkA    => open,
      clkB         => dac_clk,
      flagOut_clkB => flush_flag
      );

  flush_cntr : process(dac_clk)
  begin
    if rising_edge(dac_clk) then
      if (dac_reset = '1') then
        flush_count <= 0;
      elsif (flush_flag = '1' and dac_flush_event = '1') then
        flush_count <= flush_count;
      elsif (flush_flag = '1') then
        flush_count <= flush_count + 1;
      elsif (dac_flush_event = '1') then
        flush_count <= flush_count - 1;
      end if;
    end if;
  end process flush_cntr;

  input_time_proc : process (ctl_in.clk)
  begin
    if rising_edge(ctl_in.clk) then
      if (ctl_in.reset = '1') then
        tx_time_int   <= (others => '0');
        tx_time_frac  <= (others => '0');
        new_ts_flag   <= '0';
        prev_opcode   <= TimeStamped_IQ_samples_op_e;
      elsif (in_in.ready = '1' and ctl_in.is_operating = '1' and in_in.valid = '1' and transmit_busy = '0') then
        if (cont_mode = '1') then
          new_ts_flag <= '1';
        elsif in_in.opcode = TimeStamped_IQ_time_op_e then
          if in_in.som = '1' then
            if prev_opcode /= TimeStamped_IQ_time_op_e then
              new_ts_flag <= '0';
            end if;
            if tx_en_d_cclk = '0' then
              tx_time_int  <= unsigned(in_in.data(SECONDS_WIDTH_c - 1 downto 0));
            end if;
          elsif in_in.eom = '1' and tx_en_d_cclk = '0' then
            tx_time_frac <= unsigned(in_in.data(in_in.data'high downto in_in.data'high + 1 - FRACTION_WIDTH_c));
            new_ts_flag  <= '1';
          end if;  
        end if;
        if (in_in.eom = '1' and (wsi_take = '1' or ts_take = '1' or nvalid_take = '1')) then
          prev_opcode <= in_in.opcode;
        end if;
      end if;
    end if;
  end process input_time_proc;

  new_ts_flag_cdc : process(dac_clk)
    -- RD before WR, then variable becomes a register
    variable tmp : std_logic;
  begin
    if rising_edge(dac_clk) then
      new_ts_flag_dclk <= tmp;
      tmp              := new_ts_flag;
    end if;
  end process new_ts_flag_cdc;

  cont_mode <= '1' when (in_in.opcode = TimeStamped_IQ_samples_op_e and prev_opcode = TimeStamped_IQ_samples_op_e) else '0';

  cont_mode_cdc : process(dac_clk)
    -- RD before WR, then variable becomes a register
    variable tmp : std_logic;
  begin
    if rising_edge(dac_clk) then
      cont_mode_dclk <= tmp;
      tmp            := cont_mode;
    end if;
  end process cont_mode_cdc;

  time_check : process(ctl_in.clk)
  begin
    if rising_edge(ctl_in.clk) then
      if (ctl_in.reset = '1') then
        transmit_now    <= '0';
        transmit_now_r1 <= '0';
        transmit_now_re <= '0';
      elsif (ctl_in.is_operating = '1') then
        transmit_now_r1 <= transmit_now;
        transmit_now_re <= transmit_now and not(transmit_now_r1);
        if (new_ts_flag = '0') then
          transmit_now <= '0';
        elsif (((time_in.seconds = tx_time_int and time_in.fraction >= tx_time_frac) or (time_in.seconds > tx_time_int)) and new_ts_flag = '1') then
          transmit_now <= '1';
        end if;
      end if;
    end if;
  end process time_check;

  transmit_now_cdc : flag_cross_domain
    port map (
      clkA         => ctl_in.clk,
      flagIn_clkA  => transmit_now_re,
      busy_clkA    => transmit_busy,
      clkB         => dac_clk,
      flagOut_clkB => transmit_now_dclk
      );

  transmit_now_dclk_r1_proc : process(dac_clk)
  begin
    if rising_edge(dac_clk) then
      if (dac_reset = '1') then
        transmit_now_dclk_r1 <= '0';
        transmit_now_dclk_r2 <= '0';
      else
        transmit_now_dclk_r1 <= transmit_now_dclk;
        transmit_now_dclk_r2 <= transmit_now_dclk_r1;
      end if;
    end if;
  end process transmit_now_dclk_r1_proc;

  tx_en_d_proc : process(dac_clk)
  begin
    if rising_edge(dac_clk) then
      if (dac_reset = '1') then
        tx_en_d <= '0';
      elsif (dac_eom_r1 = '1' and dac_eom_r2 = '1' and (new_ts_flag_dclk = '0' or (new_ts_flag_dclk = '1' and dac_ready = '0'))) then
        tx_en_d <= '0';
        report "TX DISABLED!!!" severity NOTE;
      elsif ((transmit_now_dclk = '1' or transmit_now_dclk_r1 = '1' or transmit_now_dclk_r2 = '1') and new_ts_flag_dclk = '1' and sel_iq_r = '0' and dac_ready = '1' and dac_ready_r1 = '1') then
        tx_en_d <= '1';
        report "TX ENABLED!!!" severity NOTE;
        report "tx_time_int/frac = " & integer'image(to_integer(tx_time_int)) & " : " & real'image(real(to_integer(tx_time_frac))/SCALE_FACTOR_c) &
               " while current time is " & integer'image(to_integer(time_in.seconds)) & " : " & real'image(real(to_integer(time_in.fraction))/SCALE_FACTOR_c);
      end if;
    end if;
  end process tx_en_d_proc;

  tx_en_d_cdc : process(ctl_in.clk)
    -- RD before WR, then variable becomes a register
    variable tmp : std_logic;
  begin
    if rising_edge(ctl_in.clk) then
      tx_en_d_cclk <= tmp;
      tmp        := tx_en_d;
    end if;
  end process tx_en_d_cdc;

  tx_event_gen : process(ctl_in.clk)
  begin
    if rising_edge(ctl_in.clk) then
      tx_en_d_cclk_r1 <= tx_en_d_cclk;
    end if;
  end process tx_event_gen;

  tx_en_d_cclk_re <= (not tx_en_d_cclk_r1) and tx_en_d_cclk;
  tx_en_d_cclk_fe <= (not tx_en_d_cclk) and tx_en_d_cclk_r1;

  tx_en_proc : process(dac_clk)
  begin
    if rising_edge(dac_clk) then
      if (dac_reset = '1') then
        worker_driven_txen <= '0';
      elsif (flush_count /= 0) then
        worker_driven_txen <= '0';
      else
        worker_driven_txen <= tx_en_d and not(dac_flush_hold);
      end if;
    end if;
  end process tx_en_proc;
  
  dac_flush_hold_proc : process(dac_clk)
  begin
    if rising_edge(dac_clk) then
      if (dac_reset = '1') then
        dac_flush_hold <= '0';
      elsif (dac_flush = '1') then
        dac_flush_hold <= '1';
      elsif (sel_iq_r = '0') then
        dac_flush_hold <= '0';
      end if;
    end if;
  end process dac_flush_hold_proc;

  -- iqstream w/ DataWidth=32 formats Q in most significant bits, I in least
  -- significant (see OpenCPI_HDL_Development section on Message Payloads vs.
  -- Physical Data Width on Data Interfaces)
  in_Q <= in_in.data(31 downto 16);
  in_I <= in_in.data(15 downto 0);

  -- Transform signed Q0.15 to signed Q0.11, taking most significant 12 bits
  -- (and using the 13th bit to round)
  trunc_round_Q : entity work.trunc_round_16_to_12_signed
    port map(
      DIN  => in_Q,
      DOUT => wsi_data_Q);
  trunc_round_I : entity work.trunc_round_16_to_12_signed
    port map(
      DIN  => in_I,
      DOUT => wsi_data_I);

  flush_op        <= '1' when (in_in.opcode = TimeStamped_IQ_flush_op_e and in_in.som = '1' and in_in.eom = '1') else '0';
  wsi_data        <= flush_op & in_in.eom & wsi_data_Q & wsi_data_I;
  wsi_valid       <= '1' when ((in_in.valid = '1' and in_in.opcode = TimeStamped_IQ_samples_op_e) or flush_op) else '0';
  ts_take         <= '1' when (in_in.ready = '1' and ctl_in.is_operating = '1' and in_in.valid = '1' and in_in.opcode /= TimeStamped_IQ_samples_op_e and tx_en_d_cclk = '0' and transmit_busy = '0') else '0';
  nvalid_take     <= '1' when (in_in.ready = '1' and ctl_in.is_operating = '1' and in_in.valid = '0') else '0';
  in_out.take     <= (wsi_take and wsi_valid) or ts_take or nvalid_take;
  dac_take        <= sel_iq_r and dac_ready and dac_ready_r1 and tx_en_d;
  dac_eom         <= dac_data(24);
  dac_flush       <= dac_data(25);
  dac_flush_event <= dac_flush and dac_ready and not(sel_iq_r) and tx_en_d;
  
  fifo : util.util.dac_fifo
    generic map(width       => FIFO_WIDTH_c,
                depth       => FIFO_DEPTH_c)
    port map   (clk         => ctl_in.clk,
                reset       => ctl_in.reset,
                operating   => ctl_in.is_operating,
                wsi_ready   => in_in.ready,
                wsi_valid   => wsi_valid,
                wsi_data    => wsi_data,
                clear       => props_in.underrun_written,
                wsi_take    => wsi_take,
                underrun    => props_out.underrun,
                dac_clk     => dac_clk,
                dac_reset   => dac_reset,
                dac_take    => dac_take,
                dac_ready   => dac_ready,
                dac_data    => dac_data);
  -- lime_tx.hdl reports back when it is done processing an event. The done flag
  -- must be registered in the case that it occurs when ready_for_event_in_port_data
  -- is not set yet
  done_flag_proc : process(ctl_in.clk)
  begin
    if rising_edge(ctl_in.clk) then
      if ctl_in.reset = '1' then
        done_flag <= '0';
      else
        if event_in_out_take = '1' then                        --clear on take
          done_flag <= '0';
        elsif dev_tx_event_in.done_processing_event = '1' then --set on done
          done_flag <= '1';
        end if;
      end if;
    end if;
  end process;

  --Event port activity (connectin,pending,opcode) must be conveyed to lime_tx.hdl
  dev_tx_event_out.connected <= event_port_connected;
  
  event_pending <= ctl_in.is_operating and event_in_in.ready;
  dev_tx_event_out.event_pending <= event_pending and not done_flag;
  
  wsi_event_in_opcode_on_off <= '1' when (event_in_in.opcode = tx_event_txOn_op_e) else '0';
  dev_tx_event_out.opcode <= wsi_event_in_opcode_on_off;
  
  -- Taking an event requires
  -- 1. Port is connected
  -- 2. Event pending (port ready and operating)
  -- 3. lime_tx.hdl is not busy other non-event SPI transaction (always not
  --    busy for pin control)
  -- 4. Counter to space out TX events signals it is ready
  -- 5. lime_tx.hdl is done processing the previous event (in case of SPI
  --    control, always done for pin_control)
  event_in_out_take <= event_port_connected and
                       event_pending and
                       not dev_tx_event_in.busy and
                       ready_for_event_in_port_data and
                       done_flag;
  event_in_out.take <= event_in_out_take;

  -- this is necessary because we have to synchronize tx events from control
  -- plane clock domain to DAC clock domain (was decided to use two
  -- flip-flop synchronizer, which necessitates ensuring tx events only occur at
  -- most at 1.5X the DAC clock rate
  -- the 
  counter_to_space_out_tx_events : process(ctl_in.clk)
    -- props.in-min_num_cp_clks_per_txen_events is UShort_t
    constant init_val : UShort_t := to_ushort(1);
    variable count : UShort_t := to_ushort(1);
  begin
    if rising_edge(ctl_in.clk) then
      if ctl_in.reset = '1' then
        count := init_val;
        ready_for_event_in_port_data <= '1';
      elsif count < props_in.min_num_cp_clks_per_txen_events then
        if count = init_val then
          if event_in_in.ready = '1' then
            count := count + 1;

            -- applies backpressure to event_in
            ready_for_event_in_port_data <= '0';

          end if;
        else -- count > 1
          count := count + 1;
        end if;
      else -- count >= props_in.min_num_cp_clks_per_txen_events
        -- note this will be always be reached when
        -- props_in.min_num_cp_clks_per_txen_events is 0 or 1
        ready_for_event_in_port_data <= '1';
        --reset counter on take
        if event_in_out_take = '1' then
          count := init_val;
        end if;
      end if;
    end if;
  end process counter_to_space_out_tx_events;

  event_in_to_txen : misc_prims.misc_prims.event_in_to_txen
    port map (event_in_clk           => event_in_clk,
              event_in_reset         => event_in_clk,
              ctl_in_is_operating    => ctl_in.is_operating,
              event_in_in_reset      => event_in_in.reset,
              event_in_in_som        => '1', -- event_in_in.som,
              event_in_in_valid      => '0', -- event_in_in.valid,
              event_in_in_eom        => '1', -- event_in_in.eom,
              event_in_in_ready      => event_in_in.ready,
              event_in_out_take      => event_in_out_take,
              event_in_opcode_on_off => wsi_event_in_opcode_on_off,
              txon_pulse             => open,
              txoff_pulse            => open,
              txen                   => event_driven_txen,
              event_in_connected     => event_port_connected,
              is_operating           => open);

  dev_txen_out.txen <= event_driven_txen and worker_driven_txen;
end rtl;
