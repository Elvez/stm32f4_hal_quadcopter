#include "stm32f4xx_hal.h"
#include "cmsis_os.h"

#include <math.h>

#include "hardware.h"
#include "em7180.h"
#include "ms5803.h"
#include "uart_print.h"
#include "types.h"

static uint32_t barometer_read_pressure(ms5803_pressure_t sens);
static uint32_t barometer_read_temperature(ms5803_temperature_t sens);
static uint8_t barometer_validate_crc();
static void barometer_reset(ms5803_cmd_t reset);
static void barometer_read_prom();

// Output Queue
extern osMailQId mailMS5803ToAltHandle;

bool g_eventStatus;

uint16_t prom_coefficient[8];

// Some constants used in calculations below
//const uint64_t POW_2_33 = 8589934592ULL; // 2^33 = 8589934592
//const uint64_t POW_2_37 = 137438953472ULL; // 2^37 = 137438953472

bool MS5803_Init()
{
    barometer_reset(MS5803_CMD_RESET);
    barometer_read_prom();
    if(barometer_validate_crc()){
        g_eventStatus = true;
        return true;
    }
    else{
        g_eventStatus = false;
        return false;
    }
}

static uint32_t barometer_read_pressure(ms5803_pressure_t sens)
{
    uint8_t in_buffer[3];
    uint32_t pressure_raw = 0;
    uint8_t data_send[1];
    
    data_send[0] = (uint8_t)sens;
    HAL_I2C_Master_Transmit(&hi2c1, (uint16_t)MS5803_ADDRESS_HIGH << 1, data_send, 1, 1000);

    switch(sens)
	{ 
		case MS5803_D1_256 : osDelay(1); break; 
		case MS5803_D1_512 : osDelay(3); break; 
		case MS5803_D1_1024: osDelay(4); break; 
		case MS5803_D1_2048: osDelay(6); break; 
		case MS5803_D1_4096: osDelay(9); break; 
	}
    HAL_I2C_Mem_Read(&hi2c1, (uint16_t)MS5803_ADDRESS_HIGH << 1,  (uint8_t)MS5803_CMD_READ, 1, in_buffer, 3, 1000);
    //Sensors_I2C1_ReadRegister(MS5803_ADDRESS_HIGH, MS5803_CMD_READ, 3, in_buffer);
    //I2C_TransmitByte(MS5803_ADDRESS_HIGH, MS5803_CMD_READ); // send 2 bytes
    //I2C_Receive(MS5803_ADDRESS_HIGH, in_buffer, 3); // pointer???
    pressure_raw = ((uint32_t)in_buffer[2]) | ((uint32_t)in_buffer[1] << 8) | ((uint32_t)in_buffer[0] << 16); 
    return pressure_raw;
}

static uint32_t barometer_read_temperature(ms5803_temperature_t sens)
{
    uint8_t in_buffer[3];
    uint32_t temperature_raw = 0;
    uint8_t data_send[1];
    data_send[0] = (uint8_t)sens;
    HAL_I2C_Master_Transmit(&hi2c1, (uint16_t)MS5803_ADDRESS_HIGH << 1, data_send, 1, 1000);

    switch(sens)
	{ 
		case MS5803_D2_256 : osDelay(1); break; 
		case MS5803_D2_512 : osDelay(3); break; 
		case MS5803_D2_1024: osDelay(4); break; 
		case MS5803_D2_2048: osDelay(6); break; 
		case MS5803_D2_4096: osDelay(9); break; 
	}
    HAL_I2C_Mem_Read(&hi2c1, (uint16_t)MS5803_ADDRESS_HIGH << 1, MS5803_CMD_READ, 1, in_buffer, 3, 1000);

    temperature_raw = ((uint32_t)in_buffer[2]) | ((uint32_t)in_buffer[1] << 8) | ((uint32_t)in_buffer[0] << 16);
    
    return temperature_raw;
}

static void barometer_reset(ms5803_cmd_t reset)
{
    uint8_t data_send[1];
    data_send[0] = reset;
    HAL_I2C_Master_Transmit(&hi2c1, (uint16_t)MS5803_ADDRESS_HIGH << 1, data_send, 1, 1000);
    HAL_Delay(1000);
}

void MS5803_Read(float *pressure, float *temperature, ms5803_pressure_t pressure_sens, ms5803_temperature_t temp_sens)
{
    int32_t pressure_raw = barometer_read_pressure(pressure_sens);
    //UART_Print(" %d\n\r", pressure_raw);
    //D1 = pressure_raw;
    int32_t temperature_raw = barometer_read_temperature(temp_sens);
    //UART_Print(" %d\n\r", temperature_raw);
    //D2 = temperature_raw;
    int32_t temp_calc;
	int32_t pressure_calc;	
	int32_t dt;
    
    
    dt = temperature_raw - ((int32_t)prom_coefficient[5] << 8 );
    // Use integer division to calculate TEMP. It is necessary to cast
    // one of the operands as a signed 64-bit integer (int64_t) so there's no 
    // rollover issues in the numerator.
    temp_calc = (((int64_t)dt * prom_coefficient[6]) >> 23) + 2000;
    
    static int64_t	offset = 0;
    static int64_t	sensitivity  = 0;
    static int64_t	dt2 = 0;
    static int64_t	offset2 = 0;
    static int64_t	sensitivity2 = 0;
    
    if (temp_calc < 2000) {
		// For 14 bar model
		// If temperature is below 20.0C
		dt2 = 3 * (((int64_t)dt * dt) >> 33);
		//dt2 = (int32_t)dt2; // recast as signed 32bit integer
		offset2 = 3 * ((temp_calc-2000) * (temp_calc-2000)) / 2;
		sensitivity2 = 5 * ((temp_calc-2000) * (temp_calc-2000)) / 8;
        
        if (temp_calc < -1500) {
            // For 14 bar model
            offset2 = offset2 + 7 * ((temp_calc+1500)*(temp_calc+1500));
            sensitivity2 = sensitivity2 + 4 * ((temp_calc+1500)*(temp_calc+1500));
        }
    } 
    else { // if TEMP is > 2000 (20.0C)
		// For 14 bar model
		dt2 = 7 * ((uint64_t)dt * dt) / 137438953472ULL; // = pow(2,37)
		//dt2 = (int32_t)dt2; // recast as signed 32bit integer
		offset2 = 1 * ((temp_calc-2000) * (temp_calc-2000)) / 16;
		sensitivity2 = 0;
    }
    
    // Calculate initial offset and sensitivity
    // Notice lots of casts to int64_t to ensure that the 
    // multiplication operations don't overflow the original 16 bit and 32 bit
    // integers
	offset = ((int64_t)prom_coefficient[2] << 16) + (((prom_coefficient[4] * (int64_t)dt)) >> 7);
	sensitivity = ((int64_t)prom_coefficient[1] << 15) + (((prom_coefficient[3] * (int64_t)dt)) >> 8);
    temp_calc = temp_calc - dt2; // both should be int32_t
    offset = offset - offset2; // both should be int64_t
    sensitivity = sensitivity - sensitivity2; // both should be int64_t
	pressure_calc = (((pressure_raw * sensitivity) / 2097152) - offset) / 32768;
    
    *pressure = (float)(pressure_calc / 10.0f);
    *temperature = (float)(temp_calc / 100.0f);
}

static void barometer_read_prom()
{
    uint8_t in_buffer[2];
    uint8_t i;
    for(i = 0; i < 8; i++){
        HAL_I2C_Mem_Read(&hi2c1, (uint16_t)MS5803_ADDRESS_HIGH << 1, MS5803_CMD_PROM+ (i << 1), 1, in_buffer, 2, 1000);
        //Sensors_I2C1_ReadRegister(MS5803_ADDRESS_HIGH, MS5803_CMD_PROM + (i << 1), 2, in_buffer);
        //I2C_TransmitByte(MS5803_ADDRESS_HIGH, MS5803_CMD_PROM + (i << 1));
        //I2C_Receive(MS5803_ADDRESS_HIGH, in_buffer, 2);
        prom_coefficient[i] = (uint16_t) (in_buffer[1] | ((uint16_t)in_buffer[0] << 8));
    }
}

static uint8_t barometer_validate_crc()
{
    int cnt;
    unsigned int n_rem;
    unsigned int crc_read;
    unsigned char  n_bit;
    
    n_rem = 0x00;
    crc_read = prom_coefficient[7];
    prom_coefficient[7] = ( 0xFF00 & ( prom_coefficient[7] ) );
    
    for (cnt = 0; cnt < 16; cnt++){ // choose LSB or MSB
        if ( cnt%2 == 1 ){
            n_rem ^= (unsigned short) ( ( prom_coefficient[cnt>>1] ) & 0x00FF );
        }
        else {
            n_rem ^= (unsigned short) ( prom_coefficient[cnt>>1] >> 8 );
        }
        for ( n_bit = 8; n_bit > 0; n_bit-- ){
            if ( n_rem & ( 0x8000 ) ){
                n_rem = ( n_rem << 1 ) ^ 0x3000;
            }
            else {
                n_rem = ( n_rem << 1 );
            }
        }
    }
    
    n_rem = ( 0x000F & ( n_rem >> 12 ) );// // final 4-bit reminder is CRC code
    prom_coefficient[7] = crc_read; // restore the crc_read to its original place
    
    // return (crc_read == (n_rem ^ 0x00));
    if(crc_read == (n_rem ^ 0x00)){
        return 1;
    }
    else{
        return 0;
    }
}

// TODO: More error messages
const char *MS5803_GetErrorString(void)
{
    if(!g_eventStatus){
        return "MS5803 CRC error\n\r";
    }
    return "MS5803 probably fine\n\r";
}


void MS5803_StartTask(void const * argument)
{    
    uint32_t wakeTime = osKernelSysTick();
    uint32_t lastTime = 0;
    
    static Ms5803Altitude_t *pMs5803Altitude;
    pMs5803Altitude = osMailAlloc(mailMS5803ToAltHandle, osWaitForever);
    
    
	while(1){
        osDelayUntil(&wakeTime, 20);
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_5, GPIO_PIN_SET);
        
        wakeTime = osKernelSysTick();
        uint32_t dt = wakeTime - lastTime;
        lastTime = wakeTime;
        
        float temperature, pressure;
        // Read barometer data
        MS5803_Read(&pressure, &temperature, MS5803_D1_4096, MS5803_D2_4096);
        float altitude = (float)(44307.7 * (1.0 - (pow(pressure / 1013.25, 0.190284))));
        //UART_Print(" %.4f\n\r", altitude);
        // Calculate dt
        
        // Assign pointer
        pMs5803Altitude->altitude = altitude;
        pMs5803Altitude->dt = (float)dt * 0.001;
        // Send data by mail to altitude task
        osMailPut(mailMS5803ToAltHandle, pMs5803Altitude);
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_5, GPIO_PIN_RESET);
    }
}

