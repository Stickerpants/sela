#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <math.h>

#include "lpc.h"
#include "rice.h"

#define SHORT_MAX 32767
#define BLOCK_SIZE 2048

int check_wav_file(FILE *fp,int *sample_rate,short *channels,short *bps);

int main(int argc,char **argv)
{
	if(argc < 3)
	{
		fprintf(stderr,"Usage : %s <input.wav> <output.sela>\n",argv[0]);
		fprintf(stderr,"Supports CD quality (16 bits/sample) WAVEform audio(*.wav) files only.\n");
		return -1;
	}

	FILE *infile = fopen(argv[1],"rb");
	FILE *outfile = fopen(argv[2],"wb");
	FILE *logfile = fopen("coffs_enc.txt","w");

	if(infile == NULL || outfile == NULL)
	{
		fprintf(stderr,"Error open input or output file.Exiting.......\n");
		return -1;
	}

	//Variables and arrays
	unsigned char opt_lpc_order = 0;
	short channels,bps;
	unsigned char rice_param_ref,rice_param_residue;
	unsigned short req_shorts_ref,req_shorts_residues,samples_per_channel;
	const short Q = 20;
	const int corr = 1 << 20;
	int i,j,frame_count = 0;
	int sample_rate,read_size;
	const int frame_sync = 0xAA55FF00;
	unsigned int req_bits_ref,req_bits_residues;

	short short_samples[BLOCK_SIZE];
	short qtz_ref_coeffs[MAX_LPC_ORDER];
	short s_residues[BLOCK_SIZE];
	unsigned short unsigned_ref[MAX_LPC_ORDER];
	unsigned short encoded_ref[MAX_LPC_ORDER];
	unsigned short u_residues[BLOCK_SIZE];
	unsigned short encoded_residues[BLOCK_SIZE];
	int spar[MAX_LPC_ORDER];
	int lpc[MAX_LPC_ORDER+1];
	double qtz_samples[BLOCK_SIZE];
	double autocorr[MAX_LPC_ORDER + 1];
	double ref[MAX_LPC_ORDER];
	double lpc_mat[MAX_LPC_ORDER][MAX_LPC_ORDER];
	int int_samples[BLOCK_SIZE];
	short residues[BLOCK_SIZE];

	//Check the wav file
	int is_wav = check_wav_file(infile,&sample_rate,&channels,&bps);
	switch(is_wav)
	{
		case 1:
			fprintf(stderr,"WAV file detected.\n");
			break;
		case -1:
			fprintf(stderr,"RIFF header not found. Exiting......\n");
			return -1;
		case -2:
			fprintf(stderr,"WAVE header not found. Exiting......\n");
			return -1;
		case -3:
			fprintf(stderr,"Not a 16 bit/sample wav file. Exiting......\n");
			return -1;
		default:
			fprintf(stderr,"Some error occured. Exiting.......\n");
	}

	//Print media info
	fprintf(stderr,"Sampling Rate : %d Hz\n",sample_rate);
	fprintf(stderr,"Bits per sample : %d\n",bps);
	fprintf(stderr,"Channels : %d\n",channels);

	//Write Media info to output
	fwrite(&sample_rate,sizeof(int),1,outfile);
	fwrite(&bps,sizeof(short),1,outfile);
	fwrite((char *)&channels,sizeof(char),1,outfile);

	//Define read size
	read_size = channels * BLOCK_SIZE;
	short *buffer = (short *)malloc(sizeof(short) * read_size);

	//Main loop
	while(feof(infile) == 0)
	{
		//Read Samples from input
		size_t read = fread(buffer,sizeof(short),read_size,infile);

		samples_per_channel = read/channels;

		//Write frame syncword
		fwrite(&frame_sync,sizeof(int),1,outfile);

		for(i = 0; i < channels; i++)
		{
			//Write channel number to output
			fwrite((char *)&i,sizeof(char),1,outfile);

			//Separate channels
			for(j = 0; j < samples_per_channel; j++)
				short_samples[j] = buffer[channels * j + i];
			
			//Quantize sample data
			for(j = 0; j < samples_per_channel; j++)
				qtz_samples[j] = ((double)short_samples[j])/SHORT_MAX;

			//Calculate autocorrelation data
			acf(qtz_samples,samples_per_channel,MAX_LPC_ORDER,1,autocorr);

			//Calculate reflection coefficients
			opt_lpc_order = compute_ref_coefs(autocorr,MAX_LPC_ORDER,ref);

			//Quantize reflection coefficients
			qtz_ref_cof(ref,opt_lpc_order,qtz_ref_coeffs);

			//signed to unsigned
			signed_to_unsigned(opt_lpc_order,qtz_ref_coeffs,unsigned_ref);

			//get optimum rice param and number of bits
			rice_param_ref = get_opt_rice_param(unsigned_ref,opt_lpc_order,&req_bits_ref);

			//Encode ref coeffs
			req_bits_ref = rice_encode_block(rice_param_ref,unsigned_ref,opt_lpc_order,encoded_ref);

			//Determine shorts
			req_shorts_ref = ceil((double)(req_bits_ref)/(sizeof(unsigned short) * 8));

			//Dequantize reflection
			dqtz_ref_cof(qtz_ref_coeffs,opt_lpc_order,Q,ref);

			//Reflection to lpc
			levinson(NULL,opt_lpc_order,ref,lpc_mat);
			lpc[0] = 0;
			for(j = 0; j < opt_lpc_order; j++)
				lpc[j+1] = corr * lpc_mat[opt_lpc_order - 1][j];

			//Copy samples
			for(j = 0; j < samples_per_channel; j++)
				int_samples[j] = short_samples[j];

			//Get residues
			calc_residue(int_samples,samples_per_channel,opt_lpc_order,Q,lpc,residues);

			//signed to unsigned
			signed_to_unsigned(samples_per_channel,residues,u_residues);

			//get optimum rice param and number of bits
			rice_param_residue = get_opt_rice_param(u_residues,samples_per_channel,&req_bits_residues);

			//Encode residues
			req_bits_residues = rice_encode_block(rice_param_residue,u_residues,samples_per_channel,encoded_residues);

			//Determine shorts
			req_shorts_residues = ceil((double)(req_bits_residues)/(sizeof(unsigned short) * 8));

			//Write rice_params,bytes,encoded lpc_coeffs to output
			fwrite(&rice_param_ref,sizeof(unsigned char),1,outfile);
			fwrite(&req_shorts_ref,sizeof(unsigned short),1,outfile);
			fwrite(&opt_lpc_order,sizeof(unsigned char),1,outfile);
			fwrite(encoded_ref,sizeof(unsigned short),req_shorts_ref,outfile);

			//Write rice_params,bytes,encoded residues to output
			fwrite(&rice_param_residue,sizeof(unsigned char),1,outfile);
			fwrite(&req_shorts_residues,sizeof(unsigned short),1,outfile);
			fwrite(&samples_per_channel,sizeof(unsigned short),1,outfile);
			fwrite(encoded_residues,sizeof(unsigned short),req_shorts_residues,outfile);
		}
	}

	free(buffer);
	fclose(infile);
	fclose(outfile);
	fclose(logfile);

	return 0;
}

int check_wav_file(FILE *fp,int *sample_rate,short *channels,short *bps)
{
	fseek(fp,0,SEEK_SET);
	char header[44];
	fread(header,sizeof(char),44,fp);

	/*Check RIFF header*/
	if(strncmp(header,"RIFF",4)!=0)
		return -1;

	/*Check WAVE Header*/
	if(strncmp((header+8),"WAVE",4)!=0)
		return -2;

	/*Sample Rate*/
	*sample_rate = *((int *)(header + 24));

	/*Channels*/
	*channels = *((short *)(header + 22));

	/*Bits per sample*/
	*bps = *((short *)(header + 34));
	if(*bps != 16)
		return -3;

	return 1;	
}
