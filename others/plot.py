#!/bin/python
import numpy as np
import matplotlib
matplotlib.use('Agg')
from matplotlib.pyplot import *
import matplotlib.pyplot as plt

data = np.loadtxt('test.txt')

plt.plot(data[:,0],data[:,1])
plt.show()
