import numpy as np
import matplotlib.pyplot as plt

def plot_iq_data(filename):
    # Load raw IQ data from file
    iq_data = np.fromfile(filename, dtype=np.int16)
    
    # Separate I and Q components
    i_data = iq_data[0::2]
    q_data = iq_data[1::2]
    
    # Create time axis (assuming 1 sample per unit time)
    time_axis = np.arange(len(i_data)) / 16
    
    # Plot I and Q data
    plt.figure(figsize=(12, 6))
    plt.subplot(2, 2, 1)
    plt.plot(time_axis, i_data, color='blue')
    plt.title('I Component')
    plt.xlabel('Time (us)')
    plt.ylabel('Amplitude')
    plt.grid()

    
    plt.subplot(2, 2, 3)
    plt.plot(time_axis, q_data, color='orange')
    plt.title('Q Component')
    plt.xlabel('Time (us)')
    plt.ylabel('Amplitude')
    plt.grid()


    plt.subplot(2, 2, (2, 4))
    plt.scatter(i_data, q_data, s=1, alpha=0.5)
    plt.title('IQ Constellation')
    plt.xlabel('I Component')
    plt.ylabel('Q Component')
    plt.grid()
    plt.axis('equal')
    
    plt.tight_layout()
    plt.show()

if __name__ == "__main__":
    plot_iq_data('capture.raw')