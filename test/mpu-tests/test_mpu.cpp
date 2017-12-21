/*
 * Test for the MPU driver
 */

#include "stdio.h"
#include "stdint.h"
#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"
#include "unity_config.h"
#include "MPU.hpp"
#include "mpu/registers.hpp"
#include "mpu/types.hpp"
#include "mpu/math.hpp"


// embedded motion driver
using namespace emd;



namespace test {
/**
 * Bus type
 * */
#ifdef CONFIG_MPU_I2C
I2C_t& i2c = getI2C((i2c_port_t)CONFIG_MPU_TEST_I2CBUS_PORT);
#elif CONFIG_MPU_SPI
SPI_t& spi = getSPI((spi_host_device_t)CONFIG_MPU_TEST_SPIBUS_HOST);
spi_device_handle_t mpuSpiHandle;
#endif
/**
 * Hold state of bus init.
 * If a test fail, isBusInit stays true, so is not initialized again
 * */
bool isBusInit = false;
/**
 * MPU class modified to initialize the bus automaticaly when
 * instantiated, and close when object is destroyed.
 * Also, resets MPU on construction and destruction.
 * */
class MPU : public mpu::MPU {
 public:
    MPU() : mpu::MPU() {
        #ifdef CONFIG_MPU_I2C
        if (!isBusInit) {
            i2c.begin((gpio_num_t)CONFIG_MPU_TEST_I2CBUS_SDA_PIN, (gpio_num_t)CONFIG_MPU_TEST_I2CBUS_SCL_PIN,
                CONFIG_MPU_TEST_I2CBUS_CLOCK_HZ);
        }
        this->setBus(i2c);
        this->setAddr((mpu::mpu_i2caddr_t)(CONFIG_MPU_TEST_I2CBUS_ADDR + mpu::MPU_I2CADDRESS_AD0_LOW));

        #elif CONFIG_MPU_SPI
        if (!isBusInit) {
            spi.begin(CONFIG_MPU_TEST_SPIBUS_MOSI_PIN, CONFIG_MPU_TEST_SPIBUS_MISO_PIN,
                CONFIG_MPU_TEST_SPIBUS_SCLK_PIN);
            spi.addDevice(0, CONFIG_MPU_TEST_SPIBUS_CLOCK_HZ, CONFIG_MPU_TEST_SPIBUS_CS_PIN, &mpuSpiHandle);
        }
        this->setBus(spi);
        this->setAddr(mpuSpiHandle);
        #endif

        if (!isBusInit) isBusInit = true;
        this->reset();
    }

    ~MPU() {
        this->reset();
        #ifdef CONFIG_MPU_I2C
        (void)0;
        #elif CONFIG_MPU_SPI
        spi.removeDevice(this->getAddr());
        #endif
        this->bus->close();
        isBusInit = false;
    }
};
using MPU_t = MPU;
}  // namespace test



/** Setup gpio and ISR for interrupt pin */
static esp_err_t mpuConfigInterrupt(void (*isr)(void*), void * arg) {
    esp_err_t ret = ESP_OK;
    gpio_config_t io_config;
    io_config.pin_bit_mask = ((uint64_t) 1 << CONFIG_MPU_TEST_INTERRUPT_PIN);
    io_config.mode = GPIO_MODE_INPUT;
    io_config.pull_up_en = GPIO_PULLUP_DISABLE;
    io_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_config.intr_type = GPIO_INTR_POSEDGE;
    ret = gpio_config(&io_config);
    if (ret) return ret;
    ret = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (ret) return ret;
    ret = gpio_isr_handler_add((gpio_num_t)CONFIG_MPU_TEST_INTERRUPT_PIN, isr, arg);
    return ret;
}

static esp_err_t mpuRemoveInterrupt() {
    esp_err_t ret;
    ret = gpio_isr_handler_remove((gpio_num_t)CONFIG_MPU_TEST_INTERRUPT_PIN);
    if (ret) return ret;
    gpio_uninstall_isr_service();
    return ret;
}


/** ISR to measure sample rate */
static void IRAM_ATTR mpuInterruptCounterISR(void * arg) {
    int& count = *((int*) arg);
    count++;
}

/** Routine to measure sample rate */
static void mpuMeasureSampleRate(test::MPU_t& mpu, uint16_t rate, int numOfSamples) {
    const int threshold = 0.05 * rate; // percentage, 0.05 = 5%
    int count = 0;
    printf("> Sample rate set to %d Hz\n", rate);
    printf("> Now measuring true interrupt rate... wait %d secs\n", numOfSamples);
    TEST_ESP_OK( mpuConfigInterrupt(mpuInterruptCounterISR, (void*) &count));
    // enable raw-sensor-data-ready interrupt to propagate to interrupt pin.
    TEST_ESP_OK( mpu.writeByte(mpu::regs::INT_ENABLE, (1 << mpu::regs::INT_ENABLE_RAW_DATA_RDY_BIT)));
    vTaskDelay((numOfSamples * 1000) / portTICK_PERIOD_MS);
    TEST_ESP_OK( mpuRemoveInterrupt());
    uint16_t finalRate = count / numOfSamples;
    printf("> Final measured rate is %d Hz\n", finalRate);
    const int minRate = rate - threshold;
    const int maxRate = rate + threshold;
    TEST_ASSERT ((finalRate >= minRate) && (finalRate <= maxRate));
}




/******************************************
 * TESTS ----------------------------------
 * ***************************************/

TEST_CASE("MPU basic test", "[MPU]")
{
    test::MPU_t mpu;
    TEST_ESP_OK( !mpu.testConnection());
    // sleep
    TEST_ESP_OK( mpu.setSleep(true));
    TEST_ASSERT_TRUE( mpu.getSleep());
    TEST_ESP_OK( mpu.lastError());
    TEST_ESP_OK( mpu.setSleep(false));
    TEST_ASSERT_FALSE( mpu.getSleep())
    TEST_ESP_OK( mpu.lastError());
    // initialize
    TEST_ESP_OK( mpu.initialize());
    // clock source
    mpu::clock_src_t clock_src = mpu::CLOCK_INTERNAL;
    TEST_ESP_OK( mpu.setClockSource(clock_src));
    TEST_ASSERT_EQUAL_INT(clock_src, mpu.getClockSource());
    TEST_ESP_OK( mpu.lastError());
    clock_src = mpu::CLOCK_PLL;
    TEST_ESP_OK( mpu.setClockSource(clock_src));
    TEST_ASSERT_EQUAL_INT(clock_src, mpu.getClockSource());
    TEST_ESP_OK( mpu.lastError());
    // digital low pass filter
    mpu::dlpf_t dlpf = mpu::DLPF_10HZ;
    TEST_ESP_OK( mpu.setDigitalLowPassFilter(dlpf));
    TEST_ASSERT_EQUAL_INT(dlpf, mpu.getDigitalLowPassFilter());
    TEST_ESP_OK( mpu.lastError());
    dlpf = mpu::DLPF_188HZ;
    TEST_ESP_OK( mpu.setDigitalLowPassFilter(dlpf));
    TEST_ASSERT_EQUAL_INT(dlpf, mpu.getDigitalLowPassFilter());
    TEST_ESP_OK( mpu.lastError());
    // full scale range
    mpu::gyro_fs_t gyro_fs = mpu::GYRO_FS_500DPS;
    TEST_ESP_OK( mpu.setGyroFullScale(gyro_fs));
    TEST_ASSERT_EQUAL_INT(gyro_fs, mpu.getGyroFullScale());
    TEST_ESP_OK( mpu.lastError());
    mpu::accel_fs_t accel_fs = mpu::ACCEL_FS_16G;
    TEST_ESP_OK( mpu.setAccelFullScale(accel_fs));
    TEST_ASSERT_EQUAL_INT(accel_fs, mpu.getAccelFullScale());
    TEST_ESP_OK( mpu.lastError());
}



TEST_CASE("MPU sample rate measurement", "[MPU]")
{
    test::MPU_t mpu;
    TEST_ESP_OK( mpu.testConnection());
    TEST_ESP_OK( mpu.initialize());
    /** invalid rate/state check */
    TEST_ESP_OK( mpu.setSampleRate(0));
    TEST_ESP_OK( mpu.setSampleRate(1));
    TEST_ESP_OK( mpu.setSampleRate(1001));
    TEST_ESP_OK( mpu.setSampleRate(4000));
    TEST_ESP_OK( mpu.setSampleRate(512));
    TEST_ESP_OK( mpu.setSampleRate(258));
    #ifdef CONFIG_MPU6500
    TEST_ESP_OK( mpu.setFchoice(mpu::FCHOICE_2));
    TEST_ASSERT_EQUAL_INT( mpu::FCHOICE_2, mpu.getFchoice());
    TEST_ESP_OK( mpu.lastError());
    TEST_ESP_OK( mpu.setSampleRate(25));
    TEST_ASSERT_NOT_EQUAL( 25, mpu.getSampleRate());
    TEST_ESP_OK( mpu.lastError());
    TEST_ESP_OK( mpu.setFchoice(mpu::FCHOICE_3));
    TEST_ASSERT_EQUAL_INT( mpu::FCHOICE_3, mpu.getFchoice());
    TEST_ESP_OK( mpu.lastError());
    #endif
    /** rate measurement */
    constexpr int numOfSamples = 5;
    constexpr uint16_t rates[] = {5, 50, 100, 250, 500, 1000};
    for (auto rate : rates) {
        TEST_ESP_OK( mpu.setSampleRate(rate));
        mpuMeasureSampleRate(mpu, rate, numOfSamples);
    }
}



TEST_CASE("MPU max sample rate test", "[MPU]")
{
    test::MPU_t mpu;
    TEST_ESP_OK( mpu.testConnection());
    TEST_ESP_OK( mpu.setSleep(false));
    #ifdef CONFIG_MPU6500
    TEST_ESP_OK( mpu.setFchoice(mpu::FCHOICE_0));
    TEST_ASSERT_EQUAL_INT( mpu::FCHOICE_0, mpu.getFchoice());
    TEST_ESP_OK( mpu.lastError());
    #endif
    /* measure maximum sample rate consistency */
    uint16_t rate = mpu::SAMPLE_RATE_MAX;
    constexpr int numOfSamples = 5;
    mpuMeasureSampleRate(mpu, rate, numOfSamples);
}


TEST_CASE("MPU low power accelerometer mode test", "[MPU]")
{
    test::MPU_t mpu;
    TEST_ESP_OK( mpu.testConnection());
    TEST_ESP_OK( mpu.initialize());
    /* assert possible configuration */
    TEST_ESP_OK( mpu.setLowPowerAccelMode(true));
    TEST_ASSERT_TRUE( mpu.getLowPowerAccelMode());
    TEST_ESP_OK( mpu.lastError());
    TEST_ESP_OK( mpu.setLowPowerAccelMode(false));
    TEST_ASSERT_FALSE( mpu.getLowPowerAccelMode());
    TEST_ESP_OK( mpu.lastError());
    TEST_ESP_OK( mpu.setLowPowerAccelMode(true));
    TEST_ASSERT_TRUE( mpu.getLowPowerAccelMode());
    TEST_ESP_OK( mpu.lastError());
    /* assert sample rate */
    #if defined CONFIG_MPU6050
    constexpr mpu::lp_accel_rate_t lp_accel_rates[] = {
        mpu::LP_ACCEL_5HZ,
        mpu::LP_ACCEL_20HZ,
        mpu::LP_ACCEL_40HZ
    };
    constexpr uint16_t rates[] = {5, 20, 40};
    #elif defined CONFIG_MPU6500
    constexpr mpu::lp_accel_rate_t lp_accel_rates[] = {
        mpu::LP_ACCEL_1_95HZ,
        mpu::LP_ACCEL_31_25HZ,
        mpu::LP_ACCEL_125HZ
    };
    constexpr uint16_t rates[] = {2, 31, 125};
    #endif
    constexpr int numOfSamples = 5;
    for (int i = 0; i < (sizeof(rates) / sizeof(rates[0])); i++) {
        TEST_ESP_OK( mpu.setLowPowerAccelRate(lp_accel_rates[i]));
        TEST_ASSERT_EQUAL_INT(lp_accel_rates[i], mpu.getLowPowerAccelRate());
        TEST_ESP_OK( mpu.lastError());
        mpuMeasureSampleRate(mpu, rates[i], numOfSamples);
    }
}



TEST_CASE("MPU interrupt configuration", "[MPU]")
{
    test::MPU_t mpu;
    TEST_ESP_OK( mpu.testConnection());
    TEST_ESP_OK( mpu.initialize());
    // configurations
    mpu::int_config_t intconfig = {
        .level = mpu::INT_LVL_ACTIVE_LOW,
        .drive = mpu::INT_DRV_PUSHPULL,
        .mode = mpu::INT_MODE_LATCH,
        .clear = mpu::INT_CLEAR_STATUS_REG
    };
    TEST_ESP_OK( mpu.setInterruptConfig(intconfig));
    mpu::int_config_t retIntconfig = mpu.getInterruptConfig();
    TEST_ESP_OK( mpu.lastError());
    TEST_ASSERT( retIntconfig.level == intconfig.level);
    TEST_ASSERT( retIntconfig.drive == intconfig.drive);
    TEST_ASSERT( retIntconfig.mode  == intconfig.mode);
    TEST_ASSERT( retIntconfig.clear == intconfig.clear);
    // test all interrupt setups
    unsigned int interrupts;
    mpu::int_en_t retInterrupts;
    for (interrupts = 0; interrupts <= 0xFF; ++interrupts) {
        TEST_ESP_OK( mpu.setInterruptEnabled(interrupts));
        vTaskDelay(20 / portTICK_PERIOD_MS);
        retInterrupts = mpu.getInterruptEnabled();
        TEST_ESP_OK( mpu.lastError());
        if (interrupts == retInterrupts) {
            printf("(0x%X) > OK\n", interrupts);
        } else {
            printf("(0x%X) > Incompatible interrupt setup, actual: 0x%X\n", interrupts, retInterrupts);
        }
    }
}



TEST_CASE("MPU basic auxiliary I2C configuration", "[MPU]")
{
    test::MPU_t mpu;
    TEST_ESP_OK( mpu.testConnection());
    TEST_ESP_OK( mpu.initialize());
    /* master configs */
    mpu::auxi2c_config_t auxi2cConfig;
    auxi2cConfig.clock = mpu::AUXI2C_CLOCK_258KHZ;
    auxi2cConfig.multi_master_en = true;
    auxi2cConfig.transition = mpu::AUXI2C_TRANS_STOP;
    auxi2cConfig.sample_delay = 31;
    auxi2cConfig.shadow_delay_en = true;
    auxi2cConfig.wait_for_es = false;
    TEST_ESP_OK( mpu.setAuxI2CConfig(auxi2cConfig));
    // check config
    mpu::auxi2c_config_t retAuxi2cConfig;
    retAuxi2cConfig = mpu.getAuxI2CConfig();
    TEST_ESP_OK( mpu.lastError());
    TEST_ASSERT( auxi2cConfig.clock == retAuxi2cConfig.clock);
    TEST_ASSERT( auxi2cConfig.multi_master_en == retAuxi2cConfig.multi_master_en);
    TEST_ASSERT( auxi2cConfig.transition == retAuxi2cConfig.transition);
    TEST_ASSERT( auxi2cConfig.sample_delay == retAuxi2cConfig.sample_delay);
    TEST_ASSERT( auxi2cConfig.shadow_delay_en == retAuxi2cConfig.shadow_delay_en);
    TEST_ASSERT( auxi2cConfig.wait_for_es == retAuxi2cConfig.wait_for_es);
    // i2c enabling/bypass
    TEST_ESP_OK( mpu.setAuxI2CBypass(true));
    TEST_ASSERT_TRUE( mpu.getAuxI2CBypass());
    TEST_ESP_OK( mpu.lastError());
    TEST_ESP_OK( mpu.setAuxI2CEnabled(true));
    TEST_ASSERT_TRUE( mpu.getAuxI2CEnabled());
    TEST_ASSERT_FALSE( mpu.getAuxI2CBypass());
    TEST_ESP_OK( mpu.lastError());
    /* slaves configs */
    mpu::auxi2c_slv_config_t slv0config;
    slv0config.slave = mpu::AUXI2C_SLAVE_0;
    slv0config.addr = 0x1F;
    slv0config.rw = mpu::AUXI2C_READ;
    slv0config.reg_addr = 0x07;
    slv0config.reg_dis = 0;
    slv0config.swap_en = 0;
    slv0config.rxlength = 14;
    slv0config.sample_delay_en = 0;
    TEST_ESP_OK( mpu.setAuxI2CSlaveConfig(slv0config));
    mpu::auxi2c_slv_config_t slv1config;
    slv1config.slave = mpu::AUXI2C_SLAVE_1;
    slv1config.addr = 0x19;
    slv1config.rw = mpu::AUXI2C_WRITE;
    slv1config.reg_addr = 0x50;
    slv1config.reg_dis = 1;
    slv1config.txdata = 0xFA;
    slv1config.sample_delay_en = 1;
    TEST_ESP_OK( mpu.setAuxI2CSlaveConfig(slv1config));
    // check configs
    mpu::auxi2c_slv_config_t retSlvconfig;
    retSlvconfig = mpu.getAuxI2CSlaveConfig(slv0config.slave);
    TEST_ESP_OK( mpu.lastError());
    TEST_ASSERT( slv0config.slave == retSlvconfig.slave);
    TEST_ASSERT( slv0config.addr == retSlvconfig.addr);
    TEST_ASSERT( slv0config.rw == retSlvconfig.rw);
    TEST_ASSERT( slv0config.reg_addr == retSlvconfig.reg_addr);
    TEST_ASSERT( slv0config.reg_dis == retSlvconfig.reg_dis);
    TEST_ASSERT( slv0config.swap_en == retSlvconfig.swap_en);
    TEST_ASSERT( slv0config.end_of_word == retSlvconfig.end_of_word);
    TEST_ASSERT( slv0config.rxlength == retSlvconfig.rxlength);
    TEST_ASSERT( slv0config.sample_delay_en == retSlvconfig.sample_delay_en);
    retSlvconfig = mpu.getAuxI2CSlaveConfig(slv1config.slave);
    TEST_ESP_OK( mpu.lastError());
    TEST_ASSERT( slv1config.slave == retSlvconfig.slave);
    TEST_ASSERT( slv1config.addr == retSlvconfig.addr);
    TEST_ASSERT( slv1config.rw == retSlvconfig.rw);
    TEST_ASSERT( slv1config.reg_addr == retSlvconfig.reg_addr);
    TEST_ASSERT( slv1config.reg_dis == retSlvconfig.reg_dis);
    TEST_ASSERT( slv1config.txdata == retSlvconfig.txdata);
    TEST_ASSERT( slv1config.sample_delay_en == retSlvconfig.sample_delay_en);
    // enable/disable slaves
    TEST_ESP_OK( mpu.setAuxI2CSlaveEnabled(slv0config.slave, true));
    TEST_ASSERT_TRUE(mpu.getAuxI2CSlaveEnabled(slv0config.slave));
    TEST_ESP_OK( mpu.lastError());
    TEST_ESP_OK( mpu.setAuxI2CSlaveEnabled(slv1config.slave, true));
    TEST_ASSERT_TRUE(mpu.getAuxI2CSlaveEnabled(slv1config.slave));
    TEST_ESP_OK( mpu.lastError());
    TEST_ESP_OK( mpu.setAuxI2CSlaveEnabled(slv0config.slave, false));
    TEST_ASSERT_FALSE(mpu.getAuxI2CSlaveEnabled(slv0config.slave));
    TEST_ESP_OK( mpu.lastError());
    TEST_ESP_OK( mpu.setAuxI2CSlaveEnabled(slv1config.slave, false));
    TEST_ASSERT_FALSE(mpu.getAuxI2CSlaveEnabled(slv1config.slave));
    TEST_ESP_OK( mpu.lastError());
}



TEST_CASE("MPU slave 4 tranfers", "[MPU]")
{
    test::MPU_t mpu;
    TEST_ESP_OK( mpu.testConnection());
    TEST_ESP_OK( mpu.initialize());
    // config master first
    mpu::auxi2c_config_t auxi2cConfig;
    auxi2cConfig.clock = mpu::AUXI2C_CLOCK_400KHZ;
    auxi2cConfig.multi_master_en = true;
    auxi2cConfig.transition = mpu::AUXI2C_TRANS_RESTART;
    auxi2cConfig.sample_delay = 0;
    auxi2cConfig.shadow_delay_en = false;
    auxi2cConfig.wait_for_es = false;
    TEST_ESP_OK( mpu.setAuxI2CConfig(auxi2cConfig));
    TEST_ESP_OK( mpu.setAuxI2CEnabled(true));
    // check for error codes
    uint8_t slaveAddr = 0x40;
    uint8_t slaveReg  = 0x00;
    uint8_t slaveOutput = 0x16;
    uint8_t slaveInput  = 0x00;
    TEST_ESP_ERR( ESP_ERR_NOT_FOUND, mpu.auxI2CReadByte(slaveAddr, slaveReg, &slaveInput));
    TEST_ESP_ERR( ESP_ERR_NOT_FOUND, mpu.auxI2CWriteByte(slaveAddr, slaveReg, slaveOutput));
    // try transfers with compass if there is
    #if defined CONFIG_MPU9250 || defined CONFIG_MPU9150
    constexpr uint8_t compassWIA = 0x48;
    slaveAddr = 0xC;
    slaveReg  = 0x0;
    TEST_ESP_OK( mpu.auxI2CReadByte(slaveAddr, slaveReg, &slaveInput));
    TEST_ASSERT_EQUAL_UINT8( compassWIA, slaveInput);
    #endif
}



TEST_CASE("MPU external frame synchronization (FSYNC pin)", "[MPU]")
{
    test::MPU_t mpu;
    TEST_ESP_OK( mpu.testConnection());
    TEST_ESP_OK( mpu.initialize());
    // config esp32 gpio for FSYNC signal simulation
    gpio_config_t fsyncIOconfig;
    fsyncIOconfig.pin_bit_mask = ((uint64_t) 1 << CONFIG_MPU_TEST_FSYNC_PIN);
    fsyncIOconfig.mode = GPIO_MODE_OUTPUT;
    fsyncIOconfig.pull_up_en = GPIO_PULLUP_DISABLE;
    fsyncIOconfig.pull_down_en = GPIO_PULLDOWN_ENABLE;
    fsyncIOconfig.intr_type = GPIO_INTR_DISABLE;
    TEST_ESP_OK( gpio_config(&fsyncIOconfig));
    // check fsync configs
    mpu::int_lvl_t fsyncLevel = mpu::INT_LVL_ACTIVE_LOW;
    TEST_ESP_OK( mpu.setFsyncConfig(fsyncLevel));
    TEST_ASSERT_EQUAL_INT( fsyncLevel, mpu.getFsyncConfig());
    TEST_ESP_OK( mpu.lastError());
    fsyncLevel = mpu::INT_LVL_ACTIVE_HIGH;
    TEST_ESP_OK( mpu.setFsyncConfig(fsyncLevel));
    TEST_ASSERT_EQUAL_INT( fsyncLevel, mpu.getFsyncConfig());
    TEST_ESP_OK( mpu.lastError());
    // enable fsync to cause an interrupt on register I2C_MST_STATUS
    TEST_ESP_OK( mpu.setFsyncEnabled(true));
    TEST_ASSERT_TRUE( mpu.getFsyncEnabled());
    TEST_ESP_OK( mpu.lastError());
    // enable fsync to propagate to INT pin and register INT_STATUS
    mpu::int_en_t intmask = mpu::INT_EN_I2C_MST_FSYNC;
    TEST_ESP_OK( mpu.setInterruptEnabled(intmask));
    TEST_ASSERT_EQUAL_UINT8( intmask, mpu.getInterruptEnabled());
    TEST_ESP_OK( mpu.lastError());
    
    // Output a FSYNC signal, then
    // check for the interrupt in INT_STATUS and I2C_MST_STATUS
    mpu::auxi2c_stat_t auxI2Cstatus = 0;
    mpu::int_en_t intStatus = 0;
    for (size_t i = 0; i < 10; i++) {
        gpio_set_level((gpio_num_t)CONFIG_MPU_TEST_FSYNC_PIN, 1);
        auxI2Cstatus = mpu.getAuxI2CStatus();
        TEST_ESP_OK( mpu.lastError());
        intStatus = mpu.getInterruptStatus();
        TEST_ESP_OK( mpu.lastError());
        TEST_ASSERT(auxI2Cstatus & mpu::AUXI2C_STAT_FSYNC);
        TEST_ASSERT(intStatus & intmask);
        gpio_set_level((gpio_num_t)CONFIG_MPU_TEST_FSYNC_PIN, 0);
        auxI2Cstatus = mpu.getAuxI2CStatus();
        TEST_ESP_OK( mpu.lastError());
        intStatus = mpu.getInterruptStatus();
        TEST_ESP_OK( mpu.lastError());
        TEST_ASSERT(! (auxI2Cstatus & mpu::AUXI2C_STAT_FSYNC));
        TEST_ASSERT(! (intStatus & intmask));
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}



TEST_CASE("MPU sensor data test", "[MPU]")
{
    test::MPU_t mpu;
    TEST_ESP_OK( mpu.testConnection());
    TEST_ESP_OK( mpu.initialize());
    // TODO..
}



TEST_CASE("MPU standby mode", "[MPU]")
{
    test::MPU_t mpu;
    TEST_ESP_OK( mpu.testConnection());
    TEST_ESP_OK( mpu.initialize());
    // check setups
    mpu::stby_en_t stbySensors[] = {
        mpu::STBY_EN_ACCEL_X | mpu::STBY_EN_GYRO_Y | mpu::STBY_EN_TEMP | mpu::STBY_EN_LOWPWR_GYRO_PLL_ON,
        mpu::STBY_EN_ACCEL_Y | mpu::STBY_EN_ACCEL_Z | mpu::STBY_EN_TEMP,
        mpu::STBY_EN_ACCEL_Z | mpu::STBY_EN_GYRO_X | mpu::STBY_EN_GYRO_Y,
        mpu::STBY_EN_TEMP | mpu::STBY_EN_LOWPWR_GYRO_PLL_ON,
        mpu::STBY_EN_ACCEL | mpu::STBY_EN_GYRO | mpu::STBY_EN_TEMP | mpu::STBY_EN_LOWPWR_GYRO_PLL_ON
    };
    for (auto stby : stbySensors) {
        TEST_ESP_OK( mpu.setStandbyMode(stby));
        mpu::stby_en_t retStbySensors = mpu.getStandbyMode();
        printf("stby: %#X, retStbySensors: %#X", stby, retStbySensors);
        TEST_ASSERT( stby == retStbySensors);
        uint8_t data[2];
        TEST_ESP_OK( mpu.readByte(mpu::regs::PWR_MGMT1, data));
        TEST_ESP_OK( mpu.readByte(mpu::regs::PWR_MGMT2, data+1));
        printf(" -> PWR_MGMT1: %#X, PWR_MGMT2: %#X\n", data[0], data[1]);
        TEST_ASSERT( ((stby & mpu::STBY_EN_TEMP) >> 3) == (data[0] & (1 << mpu::regs::PWR1_TEMP_DIS_BIT)));
        TEST_ASSERT( ((stby & mpu::STBY_EN_LOWPWR_GYRO_PLL_ON) >> 3) == (data[0] & (1 << mpu::regs::PWR1_GYRO_STANDBY_BIT)));
        TEST_ASSERT( (stby & mpu::STBY_EN_ACCEL) == (data[1] & mpu::regs::PWR2_STBY_XYZA_BITS));
        TEST_ASSERT( (stby & mpu::STBY_EN_GYRO) == (data[1] & mpu::regs::PWR2_STBY_XYZG_BITS));
    }
}



TEST_CASE("MPU FIFO buffer", "[MPU]")
{
    test::MPU_t mpu;
    TEST_ESP_OK( mpu.testConnection());
    TEST_ESP_OK( mpu.initialize());
    TEST_ESP_OK( mpu.setSampleRate(4));
    // config mode
    mpu::fifo_mode_t fifoMode = mpu::FIFO_MODE_STOP_FULL;
    TEST_ESP_OK( mpu.setFIFOMode(fifoMode));
    TEST_ASSERT_EQUAL_INT( fifoMode, mpu.getFIFOMode());
    TEST_ESP_OK( mpu.lastError());
    // enable fifo
    TEST_ESP_OK( mpu.setFIFOEnabled(true));
    TEST_ASSERT_TRUE( mpu.getFIFOEnabled());
    TEST_ESP_OK( mpu.lastError());
    /* prepare for test */
    #ifdef CONFIG_MPU_AK89xx
    // free slaves 0 and 1 in case of MPU9150 or MPU9250
    TEST_ESP_OK( mpu.compassSetMode(mpu::MAG_MODE_POWER_DOWN));
    #endif
    mpu::auxi2c_slv_config_t slvconfig;
    slvconfig.slave = mpu::AUXI2C_SLAVE_0;
    slvconfig.rw = mpu::AUXI2C_READ;
    slvconfig.rxlength = 2;
    TEST_ESP_OK( mpu.setAuxI2CSlaveConfig(slvconfig));
    TEST_ESP_OK( mpu.setAuxI2CSlaveEnabled(slvconfig.slave, true));
    TEST_ESP_OK( mpu.setAuxI2CEnabled(true));
    TEST_ESP_OK( mpu.setInterruptEnabled(mpu::INT_EN_RAWDATA_READY));
    // sets of configs
    mpu::fifo_config_t fifoConfigs[] = {
        mpu::FIFO_CFG_ACCEL | mpu::FIFO_CFG_GYRO | mpu::FIFO_CFG_TEMPERATURE,
        mpu::FIFO_CFG_ACCEL | mpu::FIFO_CFG_TEMPERATURE,
        mpu::FIFO_CFG_GYRO,
        mpu::FIFO_CFG_SLAVE0 | mpu::FIFO_CFG_SLAVE1| mpu::FIFO_CFG_SLAVE2 | mpu::FIFO_CFG_SLAVE3
    };
    uint16_t countArray[] = { 14, 8, 6, 2};
    /* test configs */
    for (int i = 0; i < sizeof(fifoConfigs) / sizeof(fifoConfigs[0]); i++) {
        // set and check config
        TEST_ESP_OK( mpu.setFIFOConfig(fifoConfigs[i]));
        mpu::fifo_config_t retConfig = mpu.getFIFOConfig();
        TEST_ASSERT( fifoConfigs[i] == retConfig);
        TEST_ESP_OK( mpu.lastError());
        // check count
        TEST_ESP_OK( mpu.resetFIFO());  // zero count first
        mpu.getInterruptStatus();  // clear status first
        while(!(mpu.getInterruptStatus() & mpu::INT_EN_RAWDATA_READY) && !mpu.lastError()) {}
        uint16_t count = mpu.getFIFOCount();
        printf("FIFO config: 0x%X, real packet count: %d\n", fifoConfigs[i], count);
        TEST_ASSERT( countArray[i] == count);
    }
}

static inline void adjust(int16_t *axis, uint8_t adj) {
    *axis = *axis * (((((adj - 128)) * 0.5f) / 128) + 1);
}


#if defined CONFIG_MPU_AK89xx
TEST_CASE("MPU compass configuration", "[MPU]")
{
    test::MPU_t mpu;
    TEST_ESP_OK( mpu.testConnection());
    TEST_ESP_OK( mpu.initialize());
    // test
    TEST_ESP_OK( mpu.compassTestConnection());
    TEST_ASSERT( mpu.compassGetMode() == mpu::MAG_MODE_SINGLE_MEASURE);
    // check sensitivity
    #ifdef CONFIG_MPU_AK8963
    mpu::mag_sensy_t magSensy = mpu::MAG_SENSITIVITY_0_6_uT;
    TEST_ESP_OK( mpu.compassSetSensitivity(magSensy));
    TEST_ASSERT( magSensy == mpu.compassGetSensitivity());
    TEST_ESP_OK( mpu.lastError());
    magSensy = mpu::MAG_SENSITIVITY_0_15_uT;
    TEST_ESP_OK( mpu.compassSetSensitivity(magSensy));
    TEST_ASSERT( magSensy == mpu.compassGetSensitivity());
    TEST_ESP_OK( mpu.lastError());
    #endif
    // self-test
    mpu::raw_axes_t magSelfTest;
    bool selftest = mpu.compassSelfTest(&magSelfTest);
    printf("[%s] self-test: %+d %+d %+d\n",
        selftest ? (LOG_COLOR_I " OK " LOG_RESET_COLOR) : (LOG_COLOR_E "FAIL" LOG_RESET_COLOR),
        magSelfTest.x, magSelfTest.y, magSelfTest.z);
    // sensitivity adjustment
    uint8_t magAdj[3];
    TEST_ESP_OK( mpu.compassGetAdjustment(magAdj, magAdj+1, magAdj+2));
    // heading
    mpu::raw_axes_t mag;
    for (int i = 0; i < 5; i++) {
        TEST_ESP_OK( mpu.heading(&mag));
        mag.x = mpu::math::magAdjust(mag.x, magAdj[0]);
        mag.y = mpu::math::magAdjust(mag.y, magAdj[1]);
        mag.z = mpu::math::magAdjust(mag.z, magAdj[2]);
        printf("heading: %+d %+d %+d\n", mag.x, mag.y, mag.z);
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}
#endif